#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#define LIBRARY_PATH "/usr/local/share/metal_smoke.metallib"
#define THREADS 256u
#define GROUP 64u

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
    say("METAL-SMOKE: [%d/7] FAIL: %s\n", step, what.UTF8String);
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

static int run(void)
{
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) {
        diagnose_discovery();
        return fail(1, @"MTLCreateSystemDefaultDevice returned nil");
    }
    say("METAL-SMOKE: [1/7] device: PASS (%s, class %s)\n", dev.name.UTF8String,
        NSStringFromClass([(NSObject *)dev class]).UTF8String);

    id<MTLCommandQueue> queue = [dev newCommandQueue];
    if (!queue)
        return fail(2, @"newCommandQueue returned nil");
    say("METAL-SMOKE: [2/7] command queue: PASS\n");

    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@LIBRARY_PATH] error:&err];
    id<MTLFunction> fn = [lib newFunctionWithName:@"mul_add"];
    if (!fn)
        return fail(3, [NSString stringWithFormat:@"library/function: %@", err]);
    say("METAL-SMOKE: [3/7] library + function mul_add: PASS\n");

    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
    if (!pso)
        return fail(4, [NSString stringWithFormat:@"compute pipeline: %@", err]);
    say("METAL-SMOKE: [4/7] compute pipeline: PASS (max threads/group %lu)\n",
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
    say("METAL-SMOKE: [5/7] buffers: PASS (in %p out %p)\n", src, dst);

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
    say("METAL-SMOKE: [6/7] dispatch 4x64: PASS\n");

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
    say("METAL-SMOKE: [7/7] verify: PASS (%u/%u, out = in * 3 + 7)\n", good, THREADS);
    say("METAL-SMOKE: ALL PASSED\n");
    return 0;
}

int main(void)
{
    say("METAL-SMOKE: start\n");
    int rc;
    @autoreleasepool {
        rc = run();
    }
    return rc;
}
