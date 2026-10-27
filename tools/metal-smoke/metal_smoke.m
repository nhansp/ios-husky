#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurfaceRef.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#define LIBRARY_PATH "/usr/local/share/metal_smoke.metallib"
#define THREADS 256u
#define GROUP 64u
#define STEPS 13
#define RT 64u
#define STR_(x) #x
#define STR(x) STR_(x)

static void say(const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;
    fputs(buf, stdout);
    fflush(stdout);
    /* reopened per line: a descriptor opened before Metal's start-up stops reaching the console */
    int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        (void)write(fd, buf, (size_t)n);
        close(fd);
    }
}

static int fail(int step, NSString *what)
{
    say("METAL-SMOKE: [%d/%d] FAIL: %s\n", step, STEPS, what.UTF8String);
    say("METAL-SMOKE: FAILED at step %d\n", step);
    return 1;
}

@protocol MaliAcceleratorInit
- (nullable id)initWithAcceleratorPort:(io_service_t)port;
@end

static void diagnose_discovery(void)
{
    NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
    say("METAL-SMOKE: diag: MTLCopyAllDevices -> %lu\n", (unsigned long)all.count);

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAcceleratorES"));
    if (!svc) {
        say("METAL-SMOKE: diag: no IOAcceleratorES service\n");
        return;
    }
    NSString *name = CFBridgingRelease(IORegistryEntryCreateCFProperty(svc, CFSTR("MetalPluginName"), kCFAllocatorDefault, 0));
    NSString *cls = CFBridgingRelease(IORegistryEntryCreateCFProperty(svc, CFSTR("MetalPluginClassName"), kCFAllocatorDefault, 0));
    say("METAL-SMOKE: diag: service 0x%x MetalPluginName=%s MetalPluginClassName=%s\n", svc, name.UTF8String,
        cls.UTF8String);

    NSString *path = [@"/System/Library/Extensions" stringByAppendingPathComponent:[name stringByAppendingString:@".bundle"]];
    NSBundle *bundle = [NSBundle bundleWithPath:path];
    NSError *err = nil;
    BOOL loaded = [bundle loadAndReturnError:&err];
    say("METAL-SMOKE: diag: bundle %s: %s loaded=%d error=%s\n", path.UTF8String, bundle ? "found" : "missing", loaded,
        err ? err.description.UTF8String : "-");
    if (!loaded)
        return;

    Class c = [bundle classNamed:cls] ?: bundle.principalClass;
    Class base = NSClassFromString(@"_MTLDevice");
    Protocol *spi = NSProtocolFromString(@"MTLDeviceSPI");
    say("METAL-SMOKE: diag: class %s subclassOf(_MTLDevice)=%d conformsTo(MTLDeviceSPI)=%d\n",
        c ? NSStringFromClass(c).UTF8String : "nil", c && base ? [c isSubclassOfClass:base] : -1,
        c && spi ? [c conformsToProtocol:spi] : -1);
    if (!c)
        return;

    id d = [(id<MaliAcceleratorInit>)[c alloc] initWithAcceleratorPort:svc];
    say("METAL-SMOKE: diag: [%s initWithAcceleratorPort:0x%x] -> %s\n", NSStringFromClass(c).UTF8String, svc,
        d ? "device" : "nil");
    IOObjectRelease(svc);
}

static id<MTLRenderPipelineState> render_pipeline(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *vs, NSString *fs,
                                                  MTLPixelFormat format, NSError **err)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:vs];
    d.fragmentFunction = [lib newFunctionWithName:fs];
    d.colorAttachments[0].pixelFormat = format;
    if (!d.vertexFunction || !d.fragmentFunction)
        return nil;
    return [dev newRenderPipelineStateWithDescriptor:d error:err];
}

/* Draws one triangle over the lower-left half of a cleared RT x RT texture; returns the command buffer error text or nil. */
static NSString *draw(id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pso, id<MTLTexture> tex, id<MTLBuffer> pos,
                      id<MTLBuffer> color, NSUInteger vertices)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (!cb || !enc)
        return @"command buffer / render encoder";
    [enc setRenderPipelineState:pso];
    [enc setVertexBuffer:pos offset:0 atIndex:0];
    if (color)
        [enc setVertexBuffer:color offset:0 atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertices];
    [enc endEncoding];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
        (void)b;
        dispatch_semaphore_signal(done);
    }];
    [cb commit];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
        return @"render pass did not complete within 5 s";
    if (cb.status != MTLCommandBufferStatusCompleted)
        return [NSString stringWithFormat:@"render status %ld error %@", (long)cb.status, cb.error];
    return nil;
}

static int within2(int a, int b) { return a - b <= 2 && b - a <= 2; }

/* Pixel (x, y) is inside when y > x, outside when y < x; the diagonal is left to the fill rule. With colors, the
 * expected value is the barycentric blend of red (bottom-left), green (bottom-right) and blue (top-left). */
static NSString *verify(id<MTLTexture> tex, bool colors, unsigned *inside_ok)
{
    static uint8_t px[RT * RT * 4];
    memset(px, 0xcd, sizeof(px));
    [tex getBytes:px bytesPerRow:RT * 4 fromRegion:MTLRegionMake2D(0, 0, RT, RT) mipmapLevel:0];
    unsigned in_ok = 0, out_ok = 0, bad = 0, first_x = 0, first_y = 0;
    for (unsigned y = 0; y < RT; y++) {
        for (unsigned x = 0; x < RT; x++) {
            const uint8_t *p = px + (y * RT + x) * 4;
            if (y == x)
                continue;
            int want[4] = {0, 0, 255, 255};
            if (y > x && !colors) {
                want[0] = 255;
                want[2] = 255;
            } else if (y > x) {
                const float l1 = (x + 0.5f) / RT, l2 = (RT - (y + 0.5f)) / RT;
                want[0] = (int)((1.0f - l1 - l2) * 255.0f + 0.5f);
                want[1] = (int)(l1 * 255.0f + 0.5f);
                want[2] = (int)(l2 * 255.0f + 0.5f);
            }
            if (within2(p[0], want[0]) && within2(p[1], want[1]) && within2(p[2], want[2]) && within2(p[3], want[3])) {
                if (y > x)
                    in_ok++;
                else
                    out_ok++;
            } else if (bad++ == 0) {
                first_x = x;
                first_y = y;
            }
        }
    }
    *inside_ok = in_ok;
    if (!bad)
        return nil;
    const uint8_t *f = px + (first_y * RT + first_x) * 4;
    const uint8_t *c = px + ((RT / 2 + 8) * RT + RT / 2 - 8) * 4;
    return [NSString stringWithFormat:@"%u wrong, inside ok %u/%u, outside ok %u/%u; first bad (%u,%u) = %02x %02x %02x %02x; "
                                      @"(%u,%u) = %02x %02x %02x %02x",
                                      bad, in_ok, RT * (RT - 1) / 2, out_ok, RT * (RT - 1) / 2, first_x, first_y, f[0], f[1],
                                      f[2], f[3], RT / 2 - 8, RT / 2 + 8, c[0], c[1], c[2], c[3]];
}

/* About 14 MiB of post-transform positions: more than the initial 5 x 2 MiB tiler heap, so the pass only completes
 * if the kernel grows the heap on CS_TILER_OOM. The grid's triangles tile the whole target. */
#define BIG_RT 1024u
#define GRID 384u
static int heap_growth(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLRenderPipelineState> flat)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                   width:BIG_RT
                                                                                  height:BIG_RT
                                                                               mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    const NSUInteger vertices = GRID * GRID * 6;
    id<MTLBuffer> grid = [dev newBufferWithLength:vertices * 2 * sizeof(float) options:MTLResourceStorageModeShared];
    if (!tex || !grid || !grid.contents)
        return fail(11, @"1024x1024 target / grid vertex buffer");
    float *v = grid.contents;
    for (unsigned y = 0; y < GRID; y++) {
        for (unsigned x = 0; x < GRID; x++) {
            const float x0 = -1.0f + 2.0f * x / GRID, x1 = -1.0f + 2.0f * (x + 1) / GRID;
            const float y0 = -1.0f + 2.0f * y / GRID, y1 = -1.0f + 2.0f * (y + 1) / GRID;
            const float quad[12] = {x0, y0, x1, y0, x0, y1, x1, y0, x1, y1, x0, y1};
            memcpy(v, quad, sizeof(quad));
            v += 12;
        }
    }
    NSString *why = draw(queue, flat, tex, grid, nil, vertices);
    if (why)
        return fail(11, [@"heap growth pass: " stringByAppendingString:why]);
    uint8_t *px = malloc(BIG_RT * BIG_RT * 4);
    if (!px)
        return fail(11, @"readback buffer");
    [tex getBytes:px bytesPerRow:BIG_RT * 4 fromRegion:MTLRegionMake2D(0, 0, BIG_RT, BIG_RT) mipmapLevel:0];
    unsigned bad = 0, first = 0;
    for (unsigned i = 0; i < BIG_RT * BIG_RT; i++) {
        const uint8_t *p = px + i * 4;
        if (p[0] != 0xff || p[1] != 0 || p[2] != 0xff || p[3] != 0xff) {
            if (bad++ == 0)
                first = i;
        }
    }
    const uint8_t *f = px + first * 4;
    NSString *res = bad ? [NSString stringWithFormat:@"%u/%u pixels wrong, first (%u,%u) = %02x %02x %02x %02x", bad,
                                                     BIG_RT * BIG_RT, first % BIG_RT, first / BIG_RT, f[0], f[1], f[2], f[3]]
                        : nil;
    free(px);
    if (res)
        return fail(11, [@"heap growth pass: " stringByAppendingString:res]);
    say("METAL-SMOKE: [11/" STR(STEPS) "] %lu-vertex pass over the initial tiler heap: PASS (%ux%u)\n",
        (unsigned long)vertices, BIG_RT, BIG_RT);
    return 0;
}

static int no_copy_buffer(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLLibrary> lib);

/* The flat triangle into a BGRA IOSurface (what CoreAnimation renders into), read back through IOSurfaceLock. */
static int surface_target(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLLibrary> lib, id<MTLBuffer> pos)
{
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = render_pipeline(dev, lib, @"flat_vs", @"flat_fs", MTLPixelFormatBGRA8Unorm, &err);
    if (!pso)
        return fail(12, [NSString stringWithFormat:@"BGRA pipeline: %@", err]);
    NSDictionary *props = @{
        (id)kIOSurfaceWidth: @(RT), (id)kIOSurfaceHeight: @(RT), (id)kIOSurfaceBytesPerElement: @4,
        (id)kIOSurfacePixelFormat: @(0x42475241u), /* 'BGRA' */
    };
    IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!surf)
        return fail(12, @"IOSurfaceCreate");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                   width:RT
                                                                                  height:RT
                                                                               mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td iosurface:surf plane:0];
    NSString *why = tex ? draw(queue, pso, tex, pos, nil, 3) : @"newTextureWithDescriptor:iosurface:plane: returned nil";
    if (!why) {
        IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *base = IOSurfaceGetBaseAddress(surf);
        const size_t row = IOSurfaceGetBytesPerRow(surf);
        unsigned bad = 0, first_x = 0, first_y = 0;
        for (unsigned y = 0; y < RT; y++) {
            for (unsigned x = 0; x < RT; x++) {
                const uint8_t *p = base + y * row + x * 4;      /* B G R A */
                const bool inside = y > x;
                if (y == x || (p[0] == 0xff && p[1] == 0 && p[2] == (inside ? 0xff : 0) && p[3] == 0xff))
                    continue;
                if (bad++ == 0) {
                    first_x = x;
                    first_y = y;
                }
            }
        }
        const uint8_t *f = base + first_y * row + first_x * 4;
        if (bad)
            why = [NSString stringWithFormat:@"%u pixels wrong (row %zu), first (%u,%u) = %02x %02x %02x %02x", bad, row,
                                             first_x, first_y, f[0], f[1], f[2], f[3]];
        IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
    }
    const uint32_t sid = IOSurfaceGetID(surf);
    CFRelease(surf);
    if (why)
        return fail(12, [NSString stringWithFormat:@"IOSurface %u target: %@", sid, why]);
    say("METAL-SMOKE: [12/" STR(STEPS) "] flat triangle into IOSurface %u: PASS\n", sid);
    return no_copy_buffer(dev, queue, lib);
}

/* mul_add again, its output in the client's own pages (newBufferWithBytesNoCopy). */
static int no_copy_buffer(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLLibrary> lib)
{
    const size_t len = 16384;
    void *mem = NULL;
    if (posix_memalign(&mem, len, len) != 0)
        return fail(13, @"posix_memalign");
    uint32_t *dst = mem;
    for (uint32_t i = 0; i < THREADS; i++)
        dst[i] = 0xdeadbeefu;
    id<MTLBuffer> out = [dev newBufferWithBytesNoCopy:mem length:len options:MTLResourceStorageModeShared deallocator:nil];
    id<MTLBuffer> in = [dev newBufferWithLength:THREADS * 4 options:MTLResourceStorageModeShared];
    NSError *err = nil;
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"mul_add"] error:&err];
    if (!out || !in || !pso) {
        free(mem);
        return fail(13, [NSString stringWithFormat:@"no-copy buffer / pipeline: %@", err]);
    }
    uint32_t *src = in.contents;
    for (uint32_t i = 0; i < THREADS; i++)
        src[i] = 0x2000u + i * 0x7u;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:in offset:0 atIndex:0];
    [enc setBuffer:out offset:0 atIndex:1];
    [enc dispatchThreadgroups:MTLSizeMake(THREADS / GROUP, 1, 1) threadsPerThreadgroup:MTLSizeMake(GROUP, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    unsigned good = 0;
    for (uint32_t i = 0; i < THREADS; i++)
        good += dst[i] == src[i] * 3u + 7u;
    const uint32_t first = dst[0];
    out = nil;
    free(mem);
    if (cb.status != MTLCommandBufferStatusCompleted || good != THREADS)
        return fail(13, [NSString stringWithFormat:@"status %ld error %@, %u/%u correct, [0] = 0x%x", (long)cb.status,
                                                   cb.error, good, THREADS, first]);
    say("METAL-SMOKE: [13/" STR(STEPS) "] compute into a no-copy client buffer: PASS (%u/%u)\n", good, THREADS);
    return 0;
}

static int render(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLLibrary> lib)
{
    NSError *err = nil;
    id<MTLRenderPipelineState> flat = render_pipeline(dev, lib, @"flat_vs", @"flat_fs", MTLPixelFormatRGBA8Unorm, &err);
    if (!flat)
        return fail(8, [NSString stringWithFormat:@"flat render pipeline: %@", err]);
    id<MTLRenderPipelineState> colored = render_pipeline(dev, lib, @"color_vs", @"color_fs", MTLPixelFormatRGBA8Unorm, &err);
    if (!colored)
        return fail(8, [NSString stringWithFormat:@"color render pipeline: %@", err]);

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                   width:RT
                                                                                  height:RT
                                                                               mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    const float pos[6] = {-1, -1, 1, -1, -1, 1};
    const float color[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    id<MTLBuffer> pbuf = [dev newBufferWithBytes:pos length:sizeof(pos) options:MTLResourceStorageModeShared];
    id<MTLBuffer> cbuf = [dev newBufferWithBytes:color length:sizeof(color) options:MTLResourceStorageModeShared];
    if (!tex || !pbuf || !cbuf)
        return fail(8, @"render target / vertex buffers");
    say("METAL-SMOKE: [8/" STR(STEPS) "] render pipelines + %ux%u RGBA8 target: PASS\n", RT, RT);

    unsigned in_ok = 0;
    NSString *why = draw(queue, flat, tex, pbuf, nil, 3) ?: verify(tex, false, &in_ok);
    if (why)
        return fail(9, [@"flat triangle: " stringByAppendingString:why]);
    say("METAL-SMOKE: [9/" STR(STEPS) "] flat triangle: PASS (%u inside pixels, clear outside)\n", in_ok);

    why = draw(queue, colored, tex, pbuf, cbuf, 3) ?: verify(tex, true, &in_ok);
    if (why)
        return fail(10, [@"interpolated triangle: " stringByAppendingString:why]);
    say("METAL-SMOKE: [10/" STR(STEPS) "] interpolated triangle: PASS (%u inside pixels within 2/255)\n", in_ok);
    int rc = heap_growth(dev, queue, flat);
    if (rc == 0)
        rc = surface_target(dev, queue, lib, pbuf);
    return rc;
}

static int run(void)
{
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) {
        diagnose_discovery();
        return fail(1, @"MTLCreateSystemDefaultDevice returned nil");
    }
    say("METAL-SMOKE: [1/" STR(STEPS) "] device: PASS (%s, class %s)\n", dev.name.UTF8String,
        NSStringFromClass([(NSObject *)dev class]).UTF8String);

    id<MTLCommandQueue> queue = [dev newCommandQueue];
    if (!queue)
        return fail(2, @"newCommandQueue returned nil");
    say("METAL-SMOKE: [2/" STR(STEPS) "] command queue: PASS\n");

    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@LIBRARY_PATH] error:&err];
    id<MTLFunction> fn = [lib newFunctionWithName:@"mul_add"];
    if (!fn)
        return fail(3, [NSString stringWithFormat:@"library/function: %@", err]);
    say("METAL-SMOKE: [3/" STR(STEPS) "] library + function mul_add: PASS\n");

    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
    if (!pso)
        return fail(4, [NSString stringWithFormat:@"compute pipeline: %@", err]);
    say("METAL-SMOKE: [4/" STR(STEPS) "] compute pipeline: PASS (max threads/group %lu)\n",
        (unsigned long)pso.maxTotalThreadsPerThreadgroup);

    id<MTLBuffer> in = [dev newBufferWithLength:THREADS * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [dev newBufferWithLength:THREADS * 4 options:MTLResourceStorageModeShared];
    if (!in || !out || !in.contents || !out.contents)
        return fail(5, @"shared buffers");
    uint32_t *src = in.contents, *dst = out.contents;
    for (uint32_t i = 0; i < THREADS; i++) {
        src[i] = 0x1000u + i * 0x11u;
        dst[i] = 0xdeadbeefu;
    }
    say("METAL-SMOKE: [5/" STR(STEPS) "] buffers: PASS (in %p out %p)\n", src, dst);

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    if (!cb || !enc)
        return fail(6, @"command buffer / compute encoder");
    [enc setComputePipelineState:pso];
    [enc setBuffer:in offset:0 atIndex:0];
    [enc setBuffer:out offset:0 atIndex:1];
    [enc dispatchThreadgroups:MTLSizeMake(THREADS / GROUP, 1, 1) threadsPerThreadgroup:MTLSizeMake(GROUP, 1, 1)];
    [enc endEncoding];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
        (void)b;
        dispatch_semaphore_signal(done);
    }];
    [cb commit];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
        return fail(6, @"command buffer did not complete within 5 s");
    if (cb.status != MTLCommandBufferStatusCompleted)
        return fail(6, [NSString stringWithFormat:@"command buffer status %ld error %@", (long)cb.status, cb.error]);
    say("METAL-SMOKE: [6/" STR(STEPS) "] dispatch 4x64: PASS\n");

    unsigned good = 0, first_bad = THREADS;
    for (uint32_t i = 0; i < THREADS; i++) {
        if (dst[i] == src[i] * 3u + 7u)
            good++;
        else if (first_bad == THREADS)
            first_bad = i;
    }
    if (good != THREADS)
        return fail(7, [NSString stringWithFormat:@"%u/%u correct, first bad [%u] = 0x%x", good, THREADS, first_bad,
                                                  dst[first_bad]]);
    say("METAL-SMOKE: [7/" STR(STEPS) "] verify: PASS (%u/%u, out = in * 3 + 7)\n", good, THREADS);

    int rc = render(dev, queue, lib);
    if (rc == 0)
        say("METAL-SMOKE: ALL PASSED\n");
    return rc;
}

int main(void)
{
    say("METAL-SMOKE: start\n");
    int rc;
    @autoreleasepool {
        @try {
            rc = run();
        } @catch (NSException *e) {
            say("METAL-SMOKE: exception %s: %s\n", e.name.UTF8String, e.reason.UTF8String);
            rc = 1;
        }
    }
    return rc;
}
