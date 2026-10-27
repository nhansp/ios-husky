/*
 * MaliMTLObjects.h - the object classes the MaliMetal plug-in provides
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliMTLObjects.h
 *
 * Hierarchy forked from SnapMetal's AdrenoMTLObjects.h, which in turn mirrors what Apple's
 * own IOGPU-family plug-ins use (AGXMetalA13 on iOS: AGXA13FamilyDevice : IOGPUMetalDevice,
 * ...CommandQueue : IOGPUMetalCommandQueue, ...; spec 04 section 4):
 *
 *   MaliMTLDevice               : IOGPUMetalDevice : _MTLDevice          (A18: must be an _MTLDevice)
 *   MaliCommandQueue            : IOGPUMetalCommandQueue : _MTLCommandQueue
 *   MaliCommandBuffer           : IOGPUMetalCommandBuffer : _MTLCommandBuffer
 *   MaliRenderCommandEncoder    : IOGPUMetalRenderCommandEncoder
 *   MaliBlitCommandEncoder      : IOGPUMetalBlitCommandEncoder
 *   MaliComputeCommandEncoder   : IOGPUMetalComputeCommandEncoder
 *   MaliBuffer / MaliTexture    : IOGPUMetalBuffer / IOGPUMetalTexture
 *   MaliSamplerState / MaliDepthStencilState : _MTLSamplerState / _MTLDepthStencilState
 *
 * The A18 correction (verify-dsc-26.5.json A18-plugin-discovery, refuted): Metal's discovery path
 * requires isSubclassOfClass:_MTLDevice, NOT IOGPUMetalDevice. We subclass IOGPUMetalDevice, which
 * satisfies it (IOGPUMetalDevice : _MTLDevice) and reuses the selector-calling machinery
 * (shmem pools, notification queue, resource creation).
 */
#pragma once
#import "MaliMetalSPI.h"
#include "core/mali_core.h"

NS_ASSUME_NONNULL_BEGIN

@class MaliCommandBuffer, MaliExecPool, MaliInternalShaders, MaliShaderCompiler;

@interface MaliMTLDevice : IOGPUMetalDevice <MTLDeviceSPI>
- (id)compiler;
- (const mali_ctx_t *)maliCtx;
- (MaliShaderCompiler *)maliShaderCompiler;
- (MaliExecPool *)maliExecPool;
- (MaliInternalShaders *)maliInternal;
@end

@interface MaliCommandQueue : IOGPUMetalCommandQueue <MTLCommandQueue>
/* The queue-state buffer (tiler heap descriptor + geometry buffer + kernel result block, MALI_QSTATE_*),
 * allocated on the first render pass. */
- (nullable id<MTLBuffer>)maliQueueState;
/* YES exactly once per queue: the caller must put MALI_KCMD_HEAP_OP(CREATE) into the command buffer
 * it is committing (commits of one queue are serialised by the queue lock held around this call). */
- (BOOL)maliTakeHeapCreate;
@end

@interface MaliCommandBuffer : IOGPUMetalCommandBuffer <MTLCommandBuffer>
/* arenas for descriptors and command-stream chunks; buffers are retained until completion and listed
 * in the segment's resource list at commit */
- (mali_arena_t *)maliArena;
- (nullable mali_cs_t *)maliCS;                    /* created on first use */
- (void)maliUseResource:(id)resource usage:(MTLResourceUsage)usage;
- (void)maliBeginEncoder:(NSString *)kind;
- (void)maliEndEncoder;
- (void)maliFail:(NSString *)why;                  /* poison the command buffer: commit raises */
@property (nonatomic, readonly) BOOL maliEncodingFailed;
@property (nonatomic) BOOL maliUsedRender;         /* a render pass was encoded: the queue heap is needed */
@end

@interface MaliRenderCommandEncoder : IOGPUMetalRenderCommandEncoder <MTLRenderCommandEncoder> @end
@interface MaliBlitCommandEncoder : IOGPUMetalBlitCommandEncoder <MTLBlitCommandEncoder> @end
@interface MaliComputeCommandEncoder : IOGPUMetalComputeCommandEncoder <MTLComputeCommandEncoder> @end

@interface MaliBuffer : IOGPUMetalBuffer <MTLBuffer> @end

@interface MaliTexture : IOGPUMetalTexture <MTLTexture>
- (nullable mali_texture_t *)maliTexture;
/* The GPU-visible backing resource (the texture itself, or the aligned shadow buffer of an IOSurface whose rows are not
 * 64-byte multiples) and its CPU-write refill, see MaliResources.m. */
- (id)maliBackingResource;
- (void)maliPrepareForGPUUse;                      /* refill the shadow from the IOSurface if the CPU changed it */
- (nullable id)maliShadow;                         /* MaliSurfaceShadow, or nil */
@end

@interface MaliSamplerState : _MTLSamplerState <MTLSamplerState>
- (const mali_sampler_params_t *)maliParams;
- (const uint32_t *)maliDesc:(BOOL)integerTexture; /* the 8-word SAMPLER descriptor */
@end

@interface MaliDepthStencilState : _MTLDepthStencilState <MTLDepthStencilState>
- (const mali_zs_state_t *)maliZS;
@end

/* ---- allocation entry points (MaliResources.m) ---- */
id<MTLBuffer> _Nullable MaliNewBuffer(id<MTLDevice> dev, void *_Nullable bytesNoCopy, NSUInteger len, MTLResourceOptions opts,
                                      void (^_Nullable dealloc)(void *, NSUInteger));
id<MTLBuffer> _Nullable MaliNewBufferWithIOSurface(id<MTLDevice> dev, IOSurfaceRef s);
id<MTLTexture> _Nullable MaliNewTexture(id<MTLDevice> dev, MTLTextureDescriptor *d, IOSurfaceRef _Nullable s, NSUInteger plane);
id<MTLTexture> _Nullable MaliNewTextureNoCopy(id<MTLDevice> dev, MTLTextureDescriptor *d, void *bytes, NSUInteger len,
                                              void (^_Nullable dealloc)(void *, NSUInteger));
/* a CPU-write-combined buffer for GPU-visible plug-in data (descriptor arenas, code pools, queue state) */
id<MTLBuffer> _Nullable MaliNewGPUBuffer(id<MTLDevice> dev, NSUInteger len, uint32_t boFlags);

/* log a dropped/refused operation once per distinct reason */
void MaliLogOnce(NSString *_Nullable reason);

NS_ASSUME_NONNULL_END
