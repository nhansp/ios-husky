/*
 * MaliMetalSPI.h - private Metal / IOGPU declarations a third-party iOS Metal driver builds against
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliMetalSPI.h
 *
 * Structure follows SnapMetal's AdrenoMetalSPI.h (the proven shape for a plug-in that
 * subclasses the IOGPU-family bases) re-verified for iOS 26.5 against the class-dump
 * headers in gpu-spec/raw/e6-spi/class-dump-headers (ipsw class-dump of the iOS DSC):
 *
 *   IOGPUMetalDevice : _MTLDevice          IOGPUMetalDevice.h
 *   IOGPUMetalCommandQueue : _MTLCommandQueue                 IOGPUMetalCommandQueue.h
 *   IOGPUMetalCommandBuffer : _MTLCommandBuffer               IOGPUMetalCommandBuffer.h
 *   IOGPUMetal{Render,Blit,Compute}CommandEncoder             IOGPUMetal*CommandEncoder.h
 *   IOGPUMetalResource / Buffer / Texture                     IOGPUMetalResource.h ...
 *
 * Spec 04 sections 2.1, 9: every selector listed here resolves on iOS with an identical
 * label. Verified-same deltas used: the buffer/texture initialisers (04 section 2.1 row
 * "IOGPUMetalBuffer/Texture"), `_segmentByteThreshold` (D13), `_res` ivar and
 * IOGPUResourceListAddResource (D14), initWithDevice:descriptor:args:argsSize: (D15).
 *
 * LINKING: _MTLDevice & friends come from Metal.framework's exports (the SDK Metal.tbd
 * lists them); IOGPUMetal* from IOGPU.framework: link with a tbd generated from the
 * iOS DSC (gpu-spec/raw/e6-spi/ios26-IOGPU.tbd, ios26-Metal.tbd) - see build.sh.
 */
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurfaceRef.h>

NS_ASSUME_NONNULL_BEGIN

/* Protocols are matched by name at runtime (conformsToProtocol:), so a local re-declaration satisfies
 * the MTLDeviceSPI assertions in -[IOGPUMetalDevice initWithAcceleratorPort:options:] and _MTLAddDevice
 * [IOGPU:0x1ccfad44c-0x1ccfad458, spec 04 D13]. */
@protocol MTLDeviceSPI <MTLDevice>
@end

/* ---- Metal.framework base classes ---- */
@interface _MTLDevice : NSObject
- (void)initLimits;
- (void)initFeatureQueries;
- (void)initWorkarounds;
- (unsigned long long)featureProfile;
@end
@interface _MTLCommandQueue : NSObject @end
@interface _MTLCommandBuffer : NSObject @end
@interface _MTLCommandEncoder : NSObject @end
@interface _MTLSamplerState : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)device samplerDescriptor:(MTLSamplerDescriptor *)desc;
@end
@interface _MTLDepthStencilState : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)device depthStencilDescriptor:(MTLDepthStencilDescriptor *)desc;
@end

/* In-process half of the shader compiler. With pluginPath "simulatorDummyPlugin" the XPC service returns
 * the specialised AIR as an MTLB container and skips dlopen of a vendor plug-in
 * [MTLCompiler:0x261faea7c, 0x261faeac4-0x261faeb4c; spec 04 D8]. */
@interface MTLCompiler : NSObject
- (nullable instancetype)initWithTargetData:(dispatch_data_t)targetData
                                  cacheUUID:(const unsigned char *)uuid32
                                 pluginPath:(NSString *)pluginPath
                                     device:(id<MTLDevice>)device
                              compilerFlags:(unsigned int)flags;
@end

/* ---- IOGPU.framework base classes ---- */
@interface IOGPUMetalDevice : _MTLDevice
- (nullable instancetype)initWithAcceleratorPort:(io_service_t)port;
- (nullable instancetype)initWithAcceleratorPort:(io_service_t)port options:(unsigned long long)options;
- (io_service_t)acceleratorPort;
@end
@interface IOGPUMetalCommandQueue : _MTLCommandQueue
- (nullable instancetype)initWithDevice:(id<MTLDevice>)device descriptor:(MTLCommandQueueDescriptor *)desc;
@end
@interface IOGPUMetalCommandBuffer : _MTLCommandBuffer
- (nullable instancetype)initWithQueue:(id<MTLCommandQueue>)queue retainedReferences:(BOOL)retained;
- (void)commit;
- (void)waitUntilCompleted;
- (void)addCompletedHandler:(MTLCommandBufferHandler)block;
- (void)endCurrentSegment;
- (nullable void *)_reserveKernelCommandBufferSpace:(NSUInteger)length;   /* returns the address inside shmem K */
- (void)beginSegment:(void *)start;
- (void *)ioGPUResourceList;                                             /* IOGPUResourceList *, embedded in the storage */
- (nullable id)akResourceList;                                           /* MTLResourceList (retained references) */
@end
@interface IOGPUMetalCommandEncoder : _MTLCommandEncoder
- (nullable instancetype)initWithCommandBuffer:(id<MTLCommandBuffer>)cb;
- (void)endEncoding;
@end
@interface IOGPUMetalRenderCommandEncoder : IOGPUMetalCommandEncoder
- (nullable instancetype)initWithCommandBuffer:(id<MTLCommandBuffer>)cb descriptor:(MTLRenderPassDescriptor *)rp;
@end
@interface IOGPUMetalBlitCommandEncoder : IOGPUMetalCommandEncoder @end
@interface IOGPUMetalComputeCommandEncoder : IOGPUMetalCommandEncoder @end
@interface IOGPUMetalResource : NSObject
- (id)device;
- (uint32_t)resourceID;
- (uint64_t)resourceSize;
- (uint64_t)gpuAddress;                  /* property gpuAddress, IOGPUMetalResource.h */
- (nullable void *)virtualAddress;       /* CPU mapping of a sysmem allocation; NULL for private storage */
@end
@interface IOGPUMetalBuffer : IOGPUMetalResource @end
@interface IOGPUMetalTexture : IOGPUMetalResource @end

extern void MTLAddDevice(id<MTLDevice> device);

/* ---- private initialisers (selector labels verified on iOS 26.6 / IOGPU 130.14-16) ---- */
@interface IOGPUMetalBuffer (MaliPrivate)
- (nullable instancetype)initWithDevice:(id)d pointer:(nullable void *)p length:(NSUInteger)len alignment:(unsigned)al
                                options:(NSUInteger)o sysMemSize:(NSUInteger)sms gpuAddress:(uint64_t)ga
                                   args:(void *)args argsSize:(unsigned)sz deallocator:(nullable id)dealloc;
- (nullable instancetype)initWithDevice:(id)d iosurface:(IOSurfaceRef)s args:(void *)args argsSize:(unsigned)sz;
@end
@interface IOGPUMetalTexture (MaliPrivate)
- (MTLTextureUsage)usage;
- (nullable instancetype)initWithDevice:(id)d descriptor:(MTLTextureDescriptor *)td iosurface:(IOSurfaceRef)s
                                  plane:(unsigned)plane field:(unsigned)field args:(void *)args argsSize:(unsigned)sz;
- (nullable instancetype)initWithDevice:(id)d descriptor:(MTLTextureDescriptor *)td sysMemSize:(uint64_t)sms
                         sysMemRowBytes:(uint64_t)rb vidMemSize:(uint64_t)vms vidMemRowBytes:(uint64_t)vrb
                                   args:(void *)args argsSize:(unsigned)sz;
- (nullable instancetype)initWithDevice:(id)d descriptor:(MTLTextureDescriptor *)td sysMemPointer:(void *)p
                             sysMemSize:(uint64_t)s sysMemLength:(uint64_t)l sysMemRowBytes:(uint64_t)rb
                                   args:(void *)a argsSize:(unsigned)sz deallocator:(nullable id)dealloc;
- (nullable instancetype)initWithTexture:(id)t pixelFormat:(MTLPixelFormat)f textureType:(MTLTextureType)tt
                                  levels:(NSRange)lv slices:(NSRange)sl;
- (nullable instancetype)initWithTexture:(id)t pixelFormat:(MTLPixelFormat)f;
- (nullable instancetype)initWithBuffer:(id)b descriptor:(MTLTextureDescriptor *)td offset:(NSUInteger)o bytesPerRow:(NSUInteger)bpr;
@end

/* ---- compiler-service output objects (Metal.framework) ---- */
typedef struct { unsigned short major; unsigned char minor, patch; } MaliArchVersion;   /* MTLTargetDeviceArchVersion */
@interface MTLTargetDeviceArchitecture : NSObject
@property (nonatomic) unsigned int cpuType;
@property (nonatomic) unsigned int subType;
@property (nonatomic) MaliArchVersion version;
@property (nonatomic) unsigned long long driverVersion;
@end
@interface MTLFunctionVariant : NSObject
- (instancetype)initWithCompilerOutput:(id)output;
@end
@interface _MTLRenderPipelineState : NSObject
- (instancetype)initWithDevice:(id)d pipelineStateDescriptor:(id)desc;
- (id<MTLDevice>)device;
@end
@interface _MTLComputePipelineState : NSObject
- (instancetype)initWithDevice:(id)d pipelineStateDescriptor:(id)desc;
- (id<MTLDevice>)device;
@end

NS_ASSUME_NONNULL_END

/* Work diagnostics (MaliCommandBuffer.m): per-process file /private/var/tmp/mali-NAME-PID.log, readable from the
 * next boot's single-user shell without launchd or logd, plus os_log. Same idea as SnapMetal's AdrenoDiagLine. */
void MaliDiagLine(NSString *_Nullable line);
