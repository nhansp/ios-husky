/*
 * MaliMTLDevice.m - the Metal device the MaliMetal plug-in registers for the Mali-G715
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliMTLDevice.m
 *
 * Discovery path (spec 05 section 2, spec 04 sections 2-3, SnapMetal AdrenoMTLDevice.m header comment for the same
 * flow on macOS):
 *   MTLRegisterDevices -> IOServiceMatching("IOAccelerator") -> registry "MetalPluginName" ("MaliMetal") and
 *   "MetalPluginClassName" ("MaliMTLDevice") -> bundle in /System/Library/Extensions -> require
 *   isSubclassOfClass:_MTLDevice (A18 correction: NOT IOGPUMetalDevice, which is a subclass of it)
 *   -> [[cls alloc] initWithAcceleratorPort:] -> MTLAddDevice() which asserts MTLDevice + MTLDeviceSPI conformance
 *   and calls -initLimits / -initFeatureQueries / -initWorkarounds. initLimits derives every limit from
 *   -featureProfile (the plug-in picks 10001, "Mac2 + Common1-3": no Apple-GPU families are claimed).
 */
#import "MaliMTLObjectsPrivate.h"
#import <CommonCrypto/CommonDigest.h>
#import <objc/runtime.h>
#import <sys/sysctl.h>
#import <os/log.h>
#import <dlfcn.h>
#import <fcntl.h>
#import <stdarg.h>
#import <unistd.h>

#ifndef MALI_FEATURE_PROFILE
#define MALI_FEATURE_PROFILE 10001ull
#endif

void MaliConsole(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof buf) n = sizeof buf - 1;
    int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) { (void)write(fd, buf, (size_t)n); close(fd); }
}

__attribute__((constructor)) static void mali_loaded(void) {
    MaliConsole("MaliMetal: loaded in %s[%d]\n", getprogname(), (int)getpid());
}

static os_log_t mali_log(void) {
    static os_log_t l;
    static dispatch_once_t o;
    dispatch_once(&o, ^{ l = os_log_create("org.mali-ios.metal", "device"); });
    return l;
}

/* ---------------------------------------------------------------------- *
 * CoreAnimation options
 *
 * With a device that answers isFramebufferReadSupported NO, CoreAnimation builds its "generic" blended pipelines
 * with blending OFF while a specialised one compiles (SnapMetal's USERSPACE-GRAPHICS note on black corners, blur and
 * backdrop). CA_DISABLE_GENERIC_SHADERS makes it wait for the specialised pipeline instead. QuartzCore reads its
 * boolean options once, at first use: set the environment variable and, if QuartzCore is already loaded, the live option
 * (CASetDebugOption, exported SPI, index found through CAGetDebugOptionEnvVariableName).
 * ---------------------------------------------------------------------- */
static void MaliSetCAOption(const char *option, const char *opposite) {
    if (getenv(option) || (opposite && getenv(opposite))) return;          /* an explicit setting wins */
    setenv(option, "1", 0);
    const char *(*optionName)(int) = (const char *(*)(int))dlsym(RTLD_DEFAULT, "CAGetDebugOptionEnvVariableName");
    void (*setOption)(int, bool) = (void (*)(int, bool))dlsym(RTLD_DEFAULT, "CASetDebugOption");
    if (!optionName || !setOption) return;                                   /* QuartzCore not loaded yet: environment only */
    for (int i = 0; i < 4096; i++) {
        const char *name = optionName(i);
        if (!name) break;
        if (strcmp(name, option)) continue;
        /* device creation can run inside CoreAnimation's own calls: set the live option from a global queue */
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ setOption(i, true); });
        return;
    }
}

/* ---------------------------------------------------------------------- *
 * Device properties: the accelerator's "MaliDevProps" CFData, else the husky ground truth
 * ---------------------------------------------------------------------- */
static void MaliLoadDevProps(io_service_t port, mali_dev_props_t *out) {
    mali_dev_props_husky_default(out);
    CFTypeRef v = port ? IORegistryEntryCreateCFProperty(port, CFSTR(MALI_PROP_DEV_PROPS), kCFAllocatorDefault, 0) : NULL;
    if (!v) { os_log(mali_log(), "no %{public}s property: husky default device properties", MALI_PROP_DEV_PROPS); return; }
    if (CFGetTypeID(v) == CFDataGetTypeID()) {
        const struct mali_dev_props_blob *b = (const void *)CFDataGetBytePtr(v);
        const CFIndex n = CFDataGetLength(v);
        if (n >= (CFIndex)offsetof(struct mali_dev_props_blob, props) && b->magic == MALI_DEV_PROPS_BLOB_MAGIC &&
            b->version == MALI_DEV_PROPS_BLOB_VERSION && b->struct_size >= 64 &&
            (CFIndex)offsetof(struct mali_dev_props_blob, props) + (CFIndex)b->struct_size <= n) {
            memcpy(out, &b->props, MIN((size_t)b->struct_size, sizeof *out));
            os_log(mali_log(), "device properties from the accelerator: gpu_id 0x%llx cores %u", out->gpu_id, out->core_count);
        } else os_log_error(mali_log(), "%{public}s is malformed (%ld bytes): husky default", MALI_PROP_DEV_PROPS, (long)n);
    }
    CFRelease(v);
}

@implementation MaliMTLDevice {
    MTLCompiler *_compiler;
    unsigned long long _featureProfile;
    mali_dev_props_t _props;
    mali_ctx_t _ctx;
    MaliShaderCompiler *_shaderCompiler;
    MaliExecPool *_execPool;
    MaliInternalShaders *_internal;
    NSLock *_lazyLock;
}

- (instancetype)initWithAcceleratorPort:(io_service_t)port {
    /* The profile must be decided before super-init: everything after MTLAddDevice reads it. */
    _featureProfile = MALI_FEATURE_PROFILE;
    const char *e = getenv("MALI_FEATURE_PROFILE");                    /* bring-up experiments only */
    if (e) _featureProfile = strtoull(e, NULL, 0);

    MaliConsole("MaliMetal: initWithAcceleratorPort 0x%x in %s\n", port, getprogname());
    self = [super initWithAcceleratorPort:port];                       /* IOGPUDeviceCreateWithOptions + shmem pools */
    if (!self) { MaliConsole("MaliMetal: IOGPUMetalDevice init failed for port 0x%x\n", port); return nil; }
    _lazyLock = [NSLock new];
    if (![self setupSegmentByteThreshold]) { MaliConsole("MaliMetal: setupSegmentByteThreshold failed\n"); return nil; }

    MaliLoadDevProps(port, &_props);
    if (mali_ctx_init(&_ctx, &_props) != 0) { MaliConsole("MaliMetal: mali_ctx_init failed\n"); return nil; }

    NSString *why = nil;
    _shaderCompiler = [[MaliShaderCompiler alloc] initWithProps:&_props error:&why];
    if (!_shaderCompiler) os_log_error(mali_log(), "air2valhall compiler unavailable: %{public}@", why);   /* pipelines then fail with the reason */
    _execPool = [[MaliExecPool alloc] initWithDevice:self];
    [self setupCompiler];                                              /* failure is not fatal for creating the device */
    if (![self isFramebufferReadSupported]) MaliSetCAOption("CA_DISABLE_GENERIC_SHADERS", "CA_FORCE_GENERIC_SHADERS");
    os_log(mali_log(), "MaliMTLDevice up: port 0x%x featureProfile %llu gpu_id 0x%llx compiler %{public}@", port, _featureProfile,
           _props.gpu_id, _shaderCompiler.compilerID ?: @"none");
    MaliDiagLine([NSString stringWithFormat:@"device up port=0x%x gpu_id=0x%llx cores=%u compiler=%@", port, _props.gpu_id, _props.core_count,
                  _shaderCompiler.compilerID ?: @"none"]);
    MaliConsole("MaliMetal: device up in %s, gpu_id 0x%llx, compiler %s\n", getprogname(), (unsigned long long)_props.gpu_id,
                (_shaderCompiler.compilerID ?: @"none").UTF8String);
    return self;
}

- (const mali_ctx_t *)maliCtx { return &_ctx; }
- (MaliShaderCompiler *)maliShaderCompiler { return _shaderCompiler; }
- (MaliExecPool *)maliExecPool { return _execPool; }
- (MaliInternalShaders *)maliInternal {
    [_lazyLock lock];
    if (!_internal) _internal = [[MaliInternalShaders alloc] initWithDevice:self];
    MaliInternalShaders *r = _internal;
    [_lazyLock unlock];
    return r;
}

/* IOGPU's device initializer stores 0 in _segmentByteThreshold, the per-segment resource byte budget
 * IOGPUResourceListInit receives; _ioGPUResourceListAddResourceEntry then refuses the first resource. Apple's own
 * paravirtual plug-in sets it to sysctl hw.memsize right after super-init (spec 04 D13); so do we. */
- (BOOL)setupSegmentByteThreshold {
    Ivar ivar = class_getInstanceVariable([IOGPUMetalDevice class], "_segmentByteThreshold");
    const char *type = ivar ? ivar_getTypeEncoding(ivar) : NULL;
    const ptrdiff_t offset = ivar ? ivar_getOffset(ivar) : 0;
    uint64_t memsize = 0;
    size_t length = sizeof memsize;
    if (!type || strcmp(type, "Q") || offset <= 0 || (size_t)offset + sizeof memsize > class_getInstanceSize([IOGPUMetalDevice class]) ||
        sysctlbyname("hw.memsize", &memsize, &length, NULL, 0) || length != sizeof memsize || !memsize) {
        os_log_error(mali_log(), "cannot set the IOGPU segment byte threshold (ivar type %{public}s)", type ?: "missing");
        return NO;
    }
    *(uint64_t *)((char *)(__bridge void *)self + offset) = memsize;
    return YES;
}

/* ---------------------------------------------------------------------- *
 * registration contract (what MTLAddDevice / initLimits read)
 * ---------------------------------------------------------------------- */
- (unsigned long long)featureProfile { return _featureProfile; }
- (MTLArgumentBuffersTier)argumentBuffersSupport { return MTLArgumentBuffersTier1; }    /* initLimits asserts 0 or 1 */

/* ---------- identity (QuartzCore lowercases vendorName and branches on "intel", spec 05 3.3) ---------- */
- (NSString *)name { return @"Mali-G715"; }
- (NSString *)vendorName { return @"ARM"; }
- (NSString *)familyName { return @"Mali"; }
- (NSString *)productName { return @"Mali-G715"; }
- (BOOL)isLowPower { return YES; }
- (BOOL)isHeadless { return NO; }
- (BOOL)isRemovable { return NO; }
- (NSUInteger)gpuCoreCount { return _props.core_count ? _props.core_count : 7; }

/* ---------- feature answers ----------
 * No Apple-GPU families, no tile shaders / imageblocks / raster order groups / SIMD-group operations / fb fetch in v1. */
- (BOOL)supportsFamily:(MTLGPUFamily)f {
    switch (f) {
        case MTLGPUFamilyCommon1: case MTLGPUFamilyCommon2: case MTLGPUFamilyCommon3: case MTLGPUFamilyMac2: return YES;
        default: return NO;
    }
}
- (BOOL)supportsTileShaders { return NO; }
- (BOOL)supportsImageBlocks { return NO; }
- (BOOL)areRasterOrderGroupsSupported { return NO; }
- (BOOL)supportsRasterOrderGroups { return NO; }
- (BOOL)supportsSIMDGroup { return NO; }
- (BOOL)supportsSIMDReduction { return NO; }
- (BOOL)supportsSIMDShuffleAndFill { return NO; }
- (BOOL)isFramebufferReadSupported { return NO; }        /* fs.uses_fb_fetch is not driven by the v1 command builder */
- (BOOL)supportsMemorylessRenderTargets { return NO; }
- (BOOL)supportsNativeHardwareFP16 { return NO; }
- (BOOL)supportsBinaryArchives { return NO; }            /* QuartzCore tolerates nil newBinaryArchiveWithDescriptor: (spec 05 3.2) */
- (BOOL)supportsDynamicLibraries { return NO; }
- (BOOL)supportsFunctionPointers { return NO; }
- (BOOL)supportsRaytracing { return NO; }
- (BOOL)supportsDestination:(long long)d { return NO; } /* spec 05 7: skip default.pipelinelib */
- (BOOL)supportsLossyCompression { return NO; }
- (BOOL)supportsExtendedXR10Formats { return NO; }
- (BOOL)supportsYCBCRFormats12 { return NO; }
- (BOOL)supportsYCBCRPackedFormats12 { return NO; }
- (BOOL)isClampToHalfBorderSupported { return NO; }
- (BOOL)isCustomBorderColorSupported { return NO; }
- (BOOL)isASTCPixelFormatsSupported { return NO; }
- (BOOL)areProgrammableSamplePositionsSupported { return NO; }
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }
- (BOOL)supportsSampleCount:(NSUInteger)n { return n == 1; }          /* MSAA is a v2 item */
- (NSUInteger)maxFragmentTextures { return MALI_MAX_TEXTURES; }
- (NSUInteger)maxFragmentSamplers { return MALI_MAX_SAMPLERS; }
- (NSUInteger)maxTextureLayers { return 2048; }
- (NSUInteger)linearTextureAlignmentBytes { return 64; }               /* pan_image pitch alignment, mali_texture.c */
/* iosurfaceTextureAlignmentBytes / iosurfaceReadOnlyTextureAlignmentBytes are deliberately not overridden: answering 64
 * made CoreImage hand a nil source to MPSImageGaussianBlur on the SnapMetal port. Unaligned IOSurface rows use aligned
 * GPU shadows instead (MaliResources.m). */
- (NSUInteger)maxThreadgroupMemoryLength { return 32768; }
- (MTLSize)maxThreadsPerThreadgroup {
    const NSUInteger n = _props.max_threads_per_wg ? _props.max_threads_per_wg : 1024;
    return MTLSizeMake(n, n, n);
}
- (NSUInteger)maxTotalComputeThreadsPerThreadgroup { return _props.max_threads_per_wg ? _props.max_threads_per_wg : 1024; }

/* ---------- required selectors neither _MTLDevice nor IOGPUMetalDevice answers (class-dump diff of MTLDeviceSPI + MTLDevice
 * protocols against the two base classes, spec 04 section 9) ---------- */
- (unsigned long long)halfFPConfig { return 0xe; }         /* the bits AGX reports on iOS 26 */
- (unsigned long long)singleFPConfig { return 0x54; }
- (unsigned long long)doubleFPConfig { return 0; }
- (BOOL)metalAssertionsEnabled { return NO; }
- (void)setMetalAssertionsEnabled:(BOOL)e {}
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)f { return 64; }
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)f { return 64; }
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)len options:(MTLResourceOptions)o {
    return (MTLSizeAndAlign){(len + MALI_PAGE_SIZE - 1) & ~(NSUInteger)(MALI_PAGE_SIZE - 1), MALI_PAGE_SIZE};
}
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d {
    const NSUInteger rb = (MAX(d.width, 1) * 16 + 63) & ~(NSUInteger)63;                 /* worst case 16 B/px */
    const NSUInteger sz = rb * MAX(d.height, 1) * MAX(d.depth, 1) * MAX(d.arrayLength, 1) * 2;
    return (MTLSizeAndAlign){(sz + MALI_PAGE_SIZE - 1) & ~(NSUInteger)(MALI_PAGE_SIZE - 1), MALI_PAGE_SIZE};
}
- (MTLSize)sparseTileSizeWithTextureType:(MTLTextureType)t pixelFormat:(MTLPixelFormat)f sampleCount:(NSUInteger)n { return MTLSizeMake(0, 0, 0); }
- (MTLSize)sparseTileSizeWithTextureType:(MTLTextureType)t pixelFormat:(MTLPixelFormat)f sampleCount:(NSUInteger)n
                          sparsePageSize:(MTLSparsePageSize)p { return MTLSizeMake(0, 0, 0); }
- (id)newIndirectCommandBufferWithDescriptor:(id)d maxCount:(NSUInteger)n options:(MTLResourceOptions)o { return nil; }
- (id)newIndirectComputeCommandEncoderWithBuffer:(id)b { return nil; }
- (id)newIndirectRenderCommandEncoderWithBuffer:(id)b { return nil; }
- (id)newIndirectArgumentEncoderWithArguments:(id)a { return nil; }
- (id)newVisibleFunctionTableWithDescriptor:(id)d { return nil; }
- (id)newLibraryWithCIFilters:(id)f imageFilterFunctionInfo:(const void *)i { return nil; }
- (id)newLibraryWithCIFiltersForComputePipeline:(id)p imageFilterFunctionInfo:(const void *)i { return nil; }
- (id)newLibraryWithGraphs:(id)g functions:(id)f error:(NSError **)e { return nil; }
- (id)newLibraryWithGraphsSPI:(id)g functions:(id)f error:(NSError **)e { return nil; }
- (id)newRenderPipelineStateWithTileDescriptor:(id)d error:(NSError **)e { return nil; }
- (id)newRenderPipelineStateWithTileDescriptor:(MTLTileRenderPipelineDescriptor *)d options:(MTLPipelineOption)o
                                    reflection:(MTLAutoreleasedRenderPipelineReflection *)r error:(NSError **)e { return nil; }
/* iOS-only selectors absent from the macOS-derived base (spec 04 section 9, SnapMetal port notes) */
- (id)newResourceGroupFromResources:(const id *)r count:(NSUInteger)n { return nil; }
- (id)newTextureLayoutWithDescriptor:(id)d isHeapOrBufferBacked:(BOOL)b { return nil; }
- (BOOL)reserveGPUAddressRange:(NSRange)r { return NO; }
- (void)reserveResourceIndicesForResourceType:(NSUInteger)t indices:(NSUInteger *)i indexCount:(NSUInteger)c {}
- (NSUInteger)resourcePatchingTypeForResourceType:(NSUInteger)t { return 0; }
- (void)unmapShaderSampleBuffer {}
- (id)copyIOSurfaceSharedTextureProperties:(id)p { return nil; }

/* Property spellings AGX implements (class-dump of AGXA13FamilyDevice, raw/e6-spi): the protocol getters above plus
 * the bare names some frameworks send. */
- (BOOL)lowPower { return YES; }
- (BOOL)headless { return NO; }
- (BOOL)removable { return NO; }
- (BOOL)depth24Stencil8PixelFormatSupported { return NO; }
- (BOOL)programmableSamplePositionsSupported { return NO; }
- (BOOL)quadDataSharingSupported { return NO; }
- (BOOL)isTileSizeIndependentVaryingsSupported { return NO; }
- (BOOL)isShaderSampleBufferMapped { return NO; }
- (BOOL)buildBVHForRIA { return NO; }
- (id)gpuRevisionName { return @"G715"; }
- (Class)renderContextClass { return [MaliRenderCommandEncoder class]; }
- (Class)blitContextClass { return [MaliBlitCommandEncoder class]; }
- (Class)computeContextClass { return [MaliComputeCommandEncoder class]; }
- (Class)threadedRenderPassClass { return Nil; }
/* AGX-only hooks IOGPU / Metal may send while a command buffer or pipeline lives; nothing to tell, nothing to store */
- (void)alertCommandBufferActivityStart {}
- (void)alertCommandBufferActivityComplete {}
- (void)_registerForSignpostEnablement {}
- (void)_deregisterForSignpostEnablement {}
- (void)_storeRenderPipelineState:(id)s {}
- (void)_storeComputePipelineState:(id)s {}
- (void)shrinkHeaps {}
- (void)setupDeferred {}
- (BOOL)disableConsistentPerfState { return NO; }
- (BOOL)enableConsistentPerfState:(int)s { return NO; }
- (id)getConsistentPerfStateInfoAndReset:(BOOL)r { return nil; }
- (id)getConsistentPerfStateMappingTable { return nil; }
- (void)removePerformanceStateAssertion:(id)a {}
- (id)counterSampleBufferLimits { return nil; }
- (id)newCounterSampleBufferWithDescriptor:(id)d error:(NSError **)e { return nil; }
- (id)newCounterSampleBufferWithLength:(NSUInteger)l label:(id)label error:(NSError **)e { return nil; }
- (BOOL)setupCompiler:(int)c { return [self setupCompiler]; }

/* ---------- shader compiler hook ----------
 * pluginPath "simulatorDummyPlugin": MTLCompilerPluginInterface::init skips dlopen of a vendor plug-in, the service
 * specialises the function and returns AIR in an MTLB container (spec 04 D8). The cache UUID is a hash of a stable
 * version string: Metal keys its on-disk function cache with it, and with this plug-in path that cache holds only
 * the service's specialised AIR, which does not depend on our back end (back-end results are cached by MaliShader). */
- (BOOL)setupCompiler {
    unsigned long long target = _props.gpu_id;                  /* opaque to Metal */
    dispatch_data_t td = dispatch_data_create(&target, sizeof target, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    unsigned char uuid[CC_SHA256_DIGEST_LENGTH];
    const char *stamp = "MaliMetal compiler cache v0.1";
    CC_SHA256(stamp, (CC_LONG)strlen(stamp), uuid);
    NSString *plugin = @"simulatorDummyPlugin";
    const char *p = getenv("MALI_COMPILER_PLUGIN");             /* bring-up experiments only */
    if (p) plugin = @(p);
    _compiler = [[MTLCompiler alloc] initWithTargetData:td cacheUUID:uuid pluginPath:plugin device:self compilerFlags:0x24];
    if (!_compiler) os_log_error(mali_log(), "MTLCompiler init failed (pluginPath %{public}@)", plugin);
    return _compiler != nil;
}
- (id)compiler { return _compiler; }

/* ---------- object creation ---------- */
/* REQUIRED overrides: _MTLDevice's newCommandQueue / ...WithMaxCommandBufferCount: / ...WithDescriptor: defaults call
 * each other forever (SnapMetal hang sample). */
- (id<MTLCommandQueue>)newCommandQueueWithDescriptor:(MTLCommandQueueDescriptor *)d {
    return [[MaliCommandQueue alloc] initWithDevice:self descriptor:d];
}
- (id<MTLCommandQueue>)newCommandQueue { return [self newCommandQueueWithDescriptor:[MTLCommandQueueDescriptor new]]; }
- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n {
    MTLCommandQueueDescriptor *d = [MTLCommandQueueDescriptor new];
    d.maxCommandBufferCount = n;
    return [self newCommandQueueWithDescriptor:d];
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)o { return MaliNewBuffer(self, NULL, len, o, nil); }
- (id<MTLBuffer>)newBufferWithBytes:(const void *)b length:(NSUInteger)len options:(MTLResourceOptions)o {
    id<MTLBuffer> buf = MaliNewBuffer(self, NULL, len, o, nil);
    if (buf && b && buf.contents) memcpy(buf.contents, b, len);
    return buf;
}
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)b length:(NSUInteger)len options:(MTLResourceOptions)o
                              deallocator:(void (^)(void *, NSUInteger))dealloc {
    return MaliNewBuffer(self, b, len, o, dealloc);
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)o gpuAddress:(unsigned long long)a { return nil; }
- (id<MTLBuffer>)newBufferWithBytes:(const void *)b length:(NSUInteger)len options:(MTLResourceOptions)o gpuAddress:(unsigned long long)a { return nil; }
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)b length:(NSUInteger)len options:(MTLResourceOptions)o gpuAddress:(unsigned long long)a
                              deallocator:(void (^)(void *, NSUInteger))dealloc { return nil; }
- (id<MTLBuffer>)newBufferWithDescriptor:(id)d { return nil; }
- (id<MTLBuffer>)newBufferWithIOSurface:(IOSurfaceRef)s { return MaliNewBufferWithIOSurface(self, s); }
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d { return MaliNewTexture(self, d, NULL, 0); }
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane {
    return MaliNewTexture(self, d, s, plane);                /* nil here aborts WindowServer (AbortWithTextureInfo) */
}
- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)b length:(NSUInteger)len descriptor:(MTLTextureDescriptor *)d
                                deallocator:(void (^)(void *, NSUInteger))dealloc {
    return MaliNewTextureNoCopy(self, d, b, len, dealloc);
}
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d {
    return [[MaliSamplerState alloc] initWithDevice:self samplerDescriptor:d];       /* must be non-nil */
}
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d {
    return [[MaliDepthStencilState alloc] initWithDevice:self depthStencilDescriptor:d];
}
- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d { return nil; }           /* QuartzCore falls back (spec 05 6 row 21) */
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)e { return nil; }
@end
