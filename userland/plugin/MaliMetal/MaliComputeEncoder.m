/*
 * MaliComputeEncoder.m - compute command encoder
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliComputeEncoder.m
 *
 * Each dispatch becomes one RUN_COMPUTE in the command buffer's stream (mali_cs_dispatch). Metal's serial dispatch
 * type orders every dispatch after the previous one, so each dispatch is followed by an encoder barrier (drain the
 * scoreboard slots + FLUSH_CACHE2, S06 3.5); MTLDispatchTypeConcurrent gets the same, which is merely stricter.
 *
 * dispatchThreads: with a grid that is not a multiple of the threadgroup uses the program variant compiled with
 * AIR2VH_F_NONUNIFORM_DISPATCH (body guarded by all(gid < threads_per_grid)), built lazily by the pipeline state.
 */
#import "MaliMTLObjectsPrivate.h"
#import "MaliBindings.h"

#define MALI_UNSUPPORTED(what) MaliLogOnce(@"compute encoder: " what " is not supported")

@implementation MaliComputeCommandEncoder {
    MaliCommandBuffer *_cb;
    BOOL _ended;
    MaliComputePipelineState *_pipe;
    mali_stage_binds_t _b;
    BOOL _constDirty;
    id<MTLTexture> _tex[MALI_MAX_TEXTURES];
    id<MTLBuffer> _buf[MALI_MAX_BUFFERS];
}

- (instancetype)initWithCommandBuffer:(id<MTLCommandBuffer>)cb {
    if (!(self = [super initWithCommandBuffer:cb])) return nil;
    _cb = (MaliCommandBuffer *)cb;
    MaliBindsInit(&_b);
    @try { [_cb maliBeginEncoder:@"compute"]; }
    @catch (NSException *e) { _ended = YES; [super endEncoding]; @throw; }
    return self;
}
- (void)maliCheckOpen {
    if (_ended || !_cb) [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: compute encoder has ended"];
}
- (void)endEncoding {
    [self maliCheckOpen];
    _ended = YES;
    @try { [_cb maliEndEncoder]; } @finally { [super endEncoding]; }
}

- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u { [self maliCheckOpen]; [_cb maliUseResource:r usage:u]; }
- (void)useResources:(const id<MTLResource> [])rs count:(NSUInteger)n usage:(MTLResourceUsage)u { for (NSUInteger i = 0; i < n; i++) [self useResource:rs[i] usage:u]; }
- (void)useHeap:(id<MTLHeap>)h { MALI_UNSUPPORTED("useHeap"); }
- (void)useHeaps:(const id<MTLHeap> [])h count:(NSUInteger)n { MALI_UNSUPPORTED("useHeaps"); }
- (void)memoryBarrierWithScope:(MTLBarrierScope)s {}                 /* every dispatch is already followed by a full barrier */
- (void)memoryBarrierWithResources:(const id<MTLResource> [])r count:(NSUInteger)n {}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)s {
    [self maliCheckOpen];
    if (s && ![s isKindOfClass:[MaliComputePipelineState class]]) { MaliLogOnce(@"setComputePipelineState: not a MaliMetal pipeline"); _pipe = nil; return; }
    _pipe = (MaliComputePipelineState *)s;
    for (id h in [_pipe maliResources]) [_cb maliUseResource:h usage:MTLResourceUsageRead];
    _constDirty = YES;
}

- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self maliCheckOpen]; if (i < MALI_MAX_BUFFERS) _buf[i] = b; MaliBindBuffer(&_b, _cb, i, b, o); }
- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < MALI_MAX_BUFFERS) MaliBindBufferOffset(&_b, i, o, _buf[i]); }
- (void)setBytes:(const void *)p length:(NSUInteger)l atIndex:(NSUInteger)i { [self maliCheckOpen]; if (i < MALI_MAX_BUFFERS) _buf[i] = nil; MaliBindBytes(&_b, _cb, i, p, l); }
- (void)setBuffers:(const id<MTLBuffer> [])bs offsets:(const NSUInteger [])os withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setBuffer:bs[k] offset:os[k] atIndex:r.location + k];
}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {
    [self maliCheckOpen];
    if (i < MALI_MAX_TEXTURES) { if (_tex[i] == t && t) return; _tex[i] = t; }
    MaliBindTexture(&_b, _cb, i, t);
}
- (void)setTextures:(const id<MTLTexture> [])ts withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setTexture:ts[k] atIndex:r.location + k]; }
- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { [self maliCheckOpen]; MaliBindSampler(&_b, i, s); _constDirty = YES; }
- (void)setSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { [self setSamplerState:s atIndex:i]; }
- (void)setSamplerStates:(const id<MTLSamplerState> [])ss withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setSamplerState:ss[k] atIndex:r.location + k]; }
- (void)setThreadgroupMemoryLength:(NSUInteger)len atIndex:(NSUInteger)i {
    [self maliCheckOpen];
    if (i >= MALI_MAX_BUFFERS || len > 32768) { MaliLogOnce(@"threadgroup memory length out of range"); return; }
    _b.tg_mem_len[i] = (uint32_t)len;
    _b.serial++;
}
- (void)setStageInRegion:(MTLRegion)r { MALI_UNSUPPORTED("stage-in regions"); }
- (void)setStageInRegionWithIndirectBuffer:(id<MTLBuffer>)b indirectBufferOffset:(NSUInteger)o { MALI_UNSUPPORTED("indirect stage-in regions"); }
- (id)newKernelDebugInfo { return nil; }
- (void)setImageblockWidth:(NSUInteger)w height:(NSUInteger)h { MALI_UNSUPPORTED("imageblocks"); }

- (void)maliDispatchGrid:(MTLSize)grid threadgroup:(MTLSize)tg threads:(MTLSize)threads nonUniform:(BOOL)nu {
    [self maliCheckOpen];
    if (!_pipe) { MaliLogOnce(@"dispatch without a (Mali) compute pipeline state: dropped"); return; }
    if (!tg.width || !tg.height || !tg.depth || !grid.width || !grid.height || !grid.depth) return;
    if (tg.width * tg.height * tg.depth > [_pipe maxTotalThreadsPerThreadgroup]) {
        MaliLogOnce([NSString stringWithFormat:@"threadgroup %lux%lux%lu exceeds the pipeline limit %lu: dispatch dropped", (unsigned long)tg.width,
                     (unsigned long)tg.height, (unsigned long)tg.depth, (unsigned long)[_pipe maxTotalThreadsPerThreadgroup]]);
        return;
    }
    MaliMTLDevice *dev = (MaliMTLDevice *)_cb.device;
    const mali_compute_pipeline_t *pp = [_pipe maliPipe];
    if (nu) {
        NSString *why = nil;
        pp = [_pipe maliNonUniformPipe:&why];
        if (!pp) { [_cb maliFail:[@"non-uniform dispatch variant: " stringByAppendingString:why ?: @"?"]]; return; }
        for (id h in [_pipe maliNonUniformResources]) [_cb maliUseResource:h usage:MTLResourceUsageRead];
    }
    if (_constDirty) {
        uint32_t n = 0;
        const MaliConstSampler *cs = [_pipe maliConstSamplers:&n];
        if (n) MaliApplyConstSamplers(&_b, cs, n);
        _constDirty = NO;
    }
    mali_cs_t *cs = [_cb maliCS];
    if (!cs) { [_cb maliFail:@"command stream allocation failed"]; return; }
    mali_dispatch_t d;
    memset(&d, 0, sizeof d);
    d.pipe = pp;
    d.grid[0] = (uint32_t)grid.width; d.grid[1] = (uint32_t)grid.height; d.grid[2] = (uint32_t)grid.depth;
    d.tg_size[0] = (uint32_t)tg.width; d.tg_size[1] = (uint32_t)tg.height; d.tg_size[2] = (uint32_t)tg.depth;
    d.threads_per_grid[0] = (uint32_t)threads.width; d.threads_per_grid[1] = (uint32_t)threads.height; d.threads_per_grid[2] = (uint32_t)threads.depth;
    d.binds = &_b;
    d.nonuniform = nu;
    d.desc_arena = [_cb maliArena];
    if (!mali_cs_dispatch(cs, [dev maliCtx], &d)) { [_cb maliFail:@"mali_cs_dispatch failed"]; return; }
    mali_cs_encoder_barrier(cs);
}
- (void)dispatchThreadgroups:(MTLSize)tgs threadsPerThreadgroup:(MTLSize)tg {
    const MTLSize total = MTLSizeMake(tgs.width * tg.width, tgs.height * tg.height, tgs.depth * tg.depth);
    [self maliDispatchGrid:tgs threadgroup:tg threads:total nonUniform:NO];
}
- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)tg {
    if (!tg.width || !tg.height || !tg.depth) return;
    const MTLSize grid = MTLSizeMake((threads.width + tg.width - 1) / tg.width, (threads.height + tg.height - 1) / tg.height,
                                     (threads.depth + tg.depth - 1) / tg.depth);
    const BOOL ragged = threads.width % tg.width || threads.height % tg.height || threads.depth % tg.depth;
    [self maliDispatchGrid:grid threadgroup:tg threads:threads nonUniform:ragged];
}
- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)b indirectBufferOffset:(NSUInteger)o threadsPerThreadgroup:(MTLSize)tg { MALI_UNSUPPORTED("indirect dispatch"); }
@end
