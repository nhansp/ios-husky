/*
 * MaliCommandBuffer.m - command queue, command buffer, kernel command stream and diagnostics
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliCommandBuffer.m
 *
 * Submission model (contract iogpu_uc_abi.h section 6/6a, MaliPluginABI.h D-1..D-4):
 *   1. encoders build one Mali command stream (CS0, mali_cs.c) from per-command-buffer descriptor/CS arenas;
 *   2. commit writes the kernel command records into shmem K:
 *        [MALI_KCMD_HEAP_OP(CREATE)]   first committed render work of a queue only (D-3)
 *        [MALI_KCMD_SUBMIT_CS]         the stream just built (D-1)
 *        MALI_KCMD_FENCE               always last (D-4)
 *      between -beginSegment: and -endCurrentSegment, with every touched resource published to the segment's
 *      resource list in between, then calls -[super commit] (IOGPU does the sel 25 submission);
 *   3. completion handlers write IOSurface shadows back and drop the arenas.
 * Pattern (reserve K space -> beginSegment -> write -> resource list -> endCurrentSegment -> super commit) is the one
 * SnapMetal's AdrenoCommandBuffer.m uses for its vendor commands; spec 04 D13/D14 confirm the selectors on iOS.
 */
#import "MaliMTLObjectsPrivate.h"
#import <os/log.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

/* ====================================================================== *
 * Diagnostics: per-process file readable from the next boot's single-user shell without launchd/logd
 * ====================================================================== */
static _Atomic uint64_t gDiagBytes;
static _Atomic unsigned gDiagLost;
static double diag_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int diag_fd(void) {
    static int fd = -1;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        char path[256];
        snprintf(path, sizeof path, "/private/var/tmp/mali-%s-%d.log", getprogname(), (int)getpid());
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd >= 0) {
            char line[160];
            const int n = snprintf(line, sizeof line, "open pid %d %s wall %ld\n", (int)getpid(), getprogname(), (long)time(NULL));
            if (n > 0) (void)write(fd, line, (size_t)n);
        }
    });
    return fd;
}
void MaliDiagLine(NSString *line) {
    if (!line) return;
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create("org.mali-ios.metal", "diag"); });
    os_log(log, "%{public}@", line);
    const int fd = diag_fd();
    if (fd < 0) { atomic_store(&gDiagLost, 1); return; }
    NSData *bytes = [[NSString stringWithFormat:@"w%ld p%d %.1f %@\n", (long)time(NULL), (int)getpid(), diag_now(), line]
                     dataUsingEncoding:NSUTF8StringEncoding];
    if (atomic_fetch_add(&gDiagBytes, bytes.length) + bytes.length > (2u << 20)) {      /* 2 MiB per process */
        if (!atomic_exchange(&gDiagLost, 1)) {
            const char marker[] = "trace-incomplete: byte budget exhausted\n";
            (void)write(fd, marker, sizeof marker - 1);
        }
        return;
    }
    if (write(fd, bytes.bytes, bytes.length) != (ssize_t)bytes.length) atomic_store(&gDiagLost, 1);
}

/* Every refused/dropped operation, once per distinct reason (at most 128 per process). */
void MaliLogOnce(NSString *reason) {
    static NSMutableSet<NSString *> *seen;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ seen = [NSMutableSet set]; });
    if (!reason) return;
    @synchronized(seen) {
        if ([seen containsObject:reason] || seen.count >= 128) return;
        [seen addObject:reason];
    }
    MaliDiagLine([@"dropped: " stringByAppendingString:reason]);
}

/* ====================================================================== *
 * Command queue
 * ====================================================================== */
@interface MaliCommandQueue (Heap)
- (void)maliHeapCreateFailed;
@end

@implementation MaliCommandQueue {
    id<MTLBuffer> _qstate;
    BOOL _heapPending;
}
/* Same shape as AGX's -initWithDevice:descriptor: (calls the IOGPU args: variant with a vendor-sized struct); our
 * queue args are the generic 0x408 bytes with no vendor tail (D-2). */
- (instancetype)initWithDevice:(id<MTLDevice>)d descriptor:(MTLCommandQueueDescriptor *)desc {
    iogpu_new_command_queue_args_t a;
    memset(&a, 0, sizeof a);
    if ((self = [super initWithDevice:d descriptor:desc args:&a argsSize:MALI_QUEUE_ARGS_SIZE])) _heapPending = YES;
    return self;
}
- (id<MTLCommandBuffer>)commandBuffer { return [[MaliCommandBuffer alloc] initWithQueue:self retainedReferences:YES]; }
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [[MaliCommandBuffer alloc] initWithQueue:self retainedReferences:NO]; }
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d {
    return [[MaliCommandBuffer alloc] initWithQueue:self retainedReferences:d.retainedReferences];
}
- (id<MTLBuffer>)maliQueueState {
    @synchronized(self) {
        if (!_qstate)
            _qstate = MaliNewGPUBuffer(self.device, MALI_QSTATE_SIZE, MALI_BO_GPU_READ | MALI_BO_GPU_WRITE | MALI_BO_CPU_MAP | MALI_BO_NONCACHEABLE);
        return _qstate;
    }
}
- (BOOL)maliTakeHeapCreate {
    @synchronized(self) {
        if (!_heapPending || !_qstate) return NO;
        _heapPending = NO;
        return YES;
    }
}
- (void)maliHeapCreateFailed { @synchronized(self) { _heapPending = YES; } }
@end

/* ====================================================================== *
 * Arena chunks over IOGPU buffers
 * ====================================================================== */
@class MaliCommandBuffer;
static bool arena_new_chunk(void *ctx, uint32_t min_size, mali_gpu_ptr_t *out, uint32_t *size_out);
static const mali_arena_ops_t kArenaOps = {.new_chunk = arena_new_chunk};

/* Writes the first pass's shadows back exactly once, from whichever of the completion handler or waitUntilCompleted
 * gets there first. */
@interface MaliShadowWriteBackOnce : NSObject
- (instancetype)initWithShadows:(NSArray *)s;
- (void)run;
@end
@implementation MaliShadowWriteBackOnce { NSArray *_shadows; BOOL _done; }
- (instancetype)initWithShadows:(NSArray *)s { if ((self = [super init])) _shadows = s; return self; }
- (void)run {
    NSArray *todo = nil;
    @synchronized(self) { if (!_done) { _done = YES; todo = _shadows; _shadows = nil; } }
    for (id sh in todo) MaliShadowWriteBack(sh);
}
@end

/* ====================================================================== *
 * Command buffer
 * ====================================================================== */
@implementation MaliCommandBuffer {
    MaliResourceUsage *_uses;
    mali_arena_t _arena;                         /* descriptors, FAU, TLS, FBD, tiler context */
    mali_arena_t _csArena;                       /* command-stream chunks */
    mali_cs_t *_cs;
    NSMutableArray<id<MTLBuffer>> *_chunks;      /* arena buffers, listed at commit */
    NSMutableArray *_held;                       /* pipelines, internal buffers kept until completion */
    NSMutableSet *_shadowWrites;
    id _writeBack;                               /* MaliShadowWriteBackOnce after commit */
    BOOL _failed;
    NSString *_activeKind;
    BOOL _usedRender;
    uint64_t _encoders, _passes;
}
@synthesize maliUsedRender = _usedRender;

- (instancetype)initWithQueue:(id<MTLCommandQueue>)q retainedReferences:(BOOL)retained {
    if ((self = [super initWithQueue:q retainedReferences:retained])) {
        mali_arena_init(&_arena, &kArenaOps, (__bridge void *)self);
        mali_arena_init(&_csArena, &kArenaOps, (__bridge void *)self);
        _chunks = [NSMutableArray array];
        _held = [NSMutableArray array];
    }
    return self;
}
- (void)dealloc { if (_cs) mali_cs_free(_cs); }

- (mali_arena_t *)maliArena { return &_arena; }
- (BOOL)maliEncodingFailed { return _failed; }
- (void)maliFail:(NSString *)why {
    _failed = YES;
    MaliDiagLine([@"command buffer failed: " stringByAppendingString:why ?: @"?"]);
}
- (mali_cs_t *)maliCS {
    if (!_cs) _cs = mali_cs_new([(MaliMTLDevice *)self.device maliCtx], &_csArena);
    return _cs;
}
- (void)maliHold:(id)object { if (object) [_held addObject:object]; }
- (NSString *)maliActiveEncoderKind { return _activeKind; }

- (void)maliBeginEncoder:(NSString *)kind {
    if (_failed || self.status >= MTLCommandBufferStatusCommitted || _activeKind) {
        _failed = YES;
        [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: command buffer cannot begin an overlapping or late encoder"];
    }
    _activeKind = kind;
    _encoders++;
}
- (void)maliEndEncoder {
    if (!_activeKind) { _failed = YES; [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: encoder ended twice"]; }
    _activeKind = nil;
}

- (MaliResourceUsage *)uses {
    if (_failed || self.status >= MTLCommandBufferStatusCommitted)
        [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: command buffer is no longer encodable"];
    if (!_uses) {
        id retained = self.retainedReferences ? [self akResourceList] : nil;
        if (self.retainedReferences && !retained)
            [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: missing retained-resource list"];
        _uses = [[MaliResourceUsage alloc] initWithDevice:self.device retainedList:retained];
    }
    return _uses;
}
- (void)maliUseResource:(id)resource usage:(MTLResourceUsage)usage {
    if (!resource) return;
    id backing = MaliGPUBacking(resource);               /* an IOSurface texture's GPU resource is its aligned shadow */
    if (backing != resource) [_held addObject:backing];
    if (usage & MTLResourceUsageWrite) [self maliNoteShadowWrite:MaliShadowOfBacking(backing)];
    [[self uses] recordResource:backing usage:usage];
}
- (void)maliNoteShadowWrite:(id)shadow {
    if (!shadow) return;
    if (!_shadowWrites) _shadowWrites = [NSMutableSet set];
    [_shadowWrites addObject:shadow];
}

/* arena callback (any thread that is encoding this command buffer) */
- (id<MTLBuffer>)maliNewChunk:(uint32_t)minSize {
    const uint32_t size = MAX(minSize, (uint32_t)MALI_CMDBUF_ARENA_CHUNK);
    id<MTLBuffer> b = MaliNewGPUBuffer(self.device, size, MALI_BO_GPU_READ | MALI_BO_GPU_WRITE | MALI_BO_CPU_MAP | MALI_BO_NONCACHEABLE);
    if (b) [_chunks addObject:b];
    return b;
}
static bool arena_new_chunk(void *ctx, uint32_t min_size, mali_gpu_ptr_t *out, uint32_t *size_out) {
    MaliCommandBuffer *cb = (__bridge MaliCommandBuffer *)ctx;
    id<MTLBuffer> b = [cb maliNewChunk:min_size];
    void *cpu = b ? b.contents : NULL;
    if (!b || !cpu) return false;
    out->cpu = cpu;
    out->gpu = [(IOGPUMetalBuffer *)b gpuAddress];
    *size_out = (uint32_t)b.length;
    return true;
}

/* ---- commit --------------------------------------------------------------------------------------------- */
- (void)commit {
    const double started = diag_now();
    @try { [self maliCommit]; }
    @finally {
        MaliDiagLine([NSString stringWithFormat:@"commit encoders=%llu passes=%llu failed=%d chunks=%lu us=%.0f", _encoders, _passes, _failed,
                      (unsigned long)_chunks.count, (diag_now() - started) * 1e6]);
    }
}

- (void)maliCommit {
    /* let the native implementation reject repeated commits without touching storage that may be in flight */
    if (self.status >= MTLCommandBufferStatusCommitted) { [super commit]; return; }
    if (_failed) [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: command buffer encoding previously failed"];
    if (_activeKind) {
        _failed = YES;
        [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: encoder was not ended before commit"];
    }

    MaliCommandQueue *queue = (MaliCommandQueue *)self.commandQueue;
    @synchronized(queue) {              /* commits of one queue are serialised: heap creation must precede its first user */
        uint64_t root = 0;
        uint32_t csize = 0;
        BOOL haveCS = NO;
        if (_cs) {
            haveCS = !mali_cs_is_empty(_cs);
            if (!mali_cs_end(_cs, &root, &csize)) { _failed = YES; [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: command stream construction failed"]; }
            if (!csize) haveCS = NO;
        }
        BOOL heap = NO;
        id<MTLBuffer> qstate = _usedRender ? [queue maliQueueState] : nil;
        if (_usedRender && haveCS) heap = [queue maliTakeHeapCreate];

        /* publish every touched resource: encoder-recorded ones are already in _uses; add arenas and the queue state */
        MaliResourceUsage *uses = [self uses];
        for (id<MTLBuffer> c in _chunks) [uses recordResource:c usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        if (haveCS && _usedRender && qstate) [uses recordResource:qstate usage:MTLResourceUsageRead | MTLResourceUsageWrite];

        const struct { uint32_t type, size; } fence = {MALI_KCMD_FENCE, sizeof fence};
        struct mali_kcmd_submit_cs submit;
        memset(&submit, 0, sizeof submit);
        submit.header.type = MALI_KCMD_SUBMIT_CS;
        submit.header.size = sizeof submit;
        submit.stream = root;
        submit.size = csize;
        submit.flush_id = 0;                      /* 0 forces the leading FLUSH_CACHE2 (S06 3.2) */
        struct mali_kcmd_heap_op hop;
        memset(&hop, 0, sizeof hop);
        if (heap) {
            hop.header.type = MALI_KCMD_HEAP_OP;
            hop.header.size = sizeof hop;
            hop.op = MALI_HEAP_OP_CREATE;
            hop.chunk_size = MALI_HEAP_CHUNK_SIZE;
            hop.initial_chunks = MALI_HEAP_INITIAL_CHUNKS;
            hop.max_chunks = MALI_HEAP_MAX_CHUNKS;
            hop.qstate_gpu_va = [(IOGPUMetalBuffer *)qstate gpuAddress];
            hop.qstate_resource_id = [(IOGPUMetalBuffer *)qstate resourceID];
        }
        const NSUInteger bytes = (heap ? sizeof hop : 0) + (haveCS ? sizeof submit : 0) + sizeof fence;
        uint8_t *cmd = [self _reserveKernelCommandBufferSpace:bytes];
        if (!cmd) { if (heap) [queue maliHeapCreateFailed]; _failed = YES; [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: kernel command allocation failed"]; }

        @try {
            [self beginSegment:cmd];              /* clears IOGPU's descriptors: publish the resources afterwards */
            NSUInteger at = 0;
            if (heap) { memcpy(cmd + at, &hop, sizeof hop); at += sizeof hop; }
            if (haveCS) { memcpy(cmd + at, &submit, sizeof submit); at += sizeof submit; }
            memcpy(cmd + at, &fence, sizeof fence);
            mali_cpu_barrier();                   /* the arenas are NC: the stream must be visible before the doorbell */
            [uses encodeIntoIOGPUList:[self ioGPUResourceList]];
            [self endCurrentSegment];
        } @catch (NSException *e) {
            if (heap) [queue maliHeapCreateFailed];
            _failed = YES;
            @throw;
        }

        if (_shadowWrites.count) {
            MaliShadowWriteBackOnce *once = [[MaliShadowWriteBackOnce alloc] initWithShadows:_shadowWrites.allObjects];
            _writeBack = once;
            [super addCompletedHandler:^(id<MTLCommandBuffer> cb) { (void)cb; [once run]; }];
        }
        /* arenas, held objects and the stream are dropped when the GPU is done with them (also on error) */
        NSArray *chunks = _chunks, *held = _held;
        mali_cs_t *cs = _cs;
        _cs = NULL;
        mali_cs_free(cs);                         /* the builder's own state; the chunk buffers live in `chunks` */
        _chunks = nil; _held = nil;
        [super addCompletedHandler:^(id<MTLCommandBuffer> cb) { (void)cb; (void)chunks; (void)held; }];
        @try { [super commit]; }
        @catch (NSException *e) { if (heap) [queue maliHeapCreateFailed]; @throw; }
    }
}

- (void)maliRunWriteBack { [(MaliShadowWriteBackOnce *)_writeBack run]; }
- (void)waitUntilCompleted {
    [super waitUntilCompleted];
    [self maliRunWriteBack];
}
/* Handlers run in the order they were added and callers add theirs before commit: each one runs the write-back first, so
 * no completion handler reads a surface before the GPU's writes reach it. */
- (void)addCompletedHandler:(MTLCommandBufferHandler)block {
    if (!block) { [super addCompletedHandler:block]; return; }
    MTLCommandBufferHandler handler = [block copy];
    [super addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        if ([(id)cb isKindOfClass:[MaliCommandBuffer class]]) [(MaliCommandBuffer *)cb maliRunWriteBack];
        handler(cb);
    }];
}

/* ---- encoders ---------------------------------------------------------------------------------------------- */
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)rp {
    _passes++;
    return [[MaliRenderCommandEncoder alloc] initWithCommandBuffer:self descriptor:rp];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder { return [[MaliBlitCommandEncoder alloc] initWithCommandBuffer:self]; }
- (id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d { return [[MaliBlitCommandEncoder alloc] initWithCommandBuffer:self]; }
- (id<MTLComputeCommandEncoder>)computeCommandEncoder { return [[MaliComputeCommandEncoder alloc] initWithCommandBuffer:self]; }
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)t { return [[MaliComputeCommandEncoder alloc] initWithCommandBuffer:self]; }
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d { return [[MaliComputeCommandEncoder alloc] initWithCommandBuffer:self]; }

/* AGX provides these (spec: plugin-class-must-implement.txt); v1 has neither parallel render nor resource-state encoders */
- (id<MTLParallelRenderCommandEncoder>)parallelRenderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
    MaliLogOnce(@"parallelRenderCommandEncoder is not supported");
    return nil;
}
- (id<MTLResourceStateCommandEncoder>)resourceStateCommandEncoder {
    MaliLogOnce(@"resourceStateCommandEncoder is not supported");
    return nil;
}

/* iOS-only stubs (spec 04 section 9): CoreAnimation sends these; there is no cache-hint hardware to tell */
- (void)encodeCacheHintTag:(id)tag resourceGroups:(const id *)groups count:(NSUInteger)n {}
- (void)encodeCacheHintFinalize:(id)tag resourceGroups:(const id *)groups count:(NSUInteger)n {}
@end
