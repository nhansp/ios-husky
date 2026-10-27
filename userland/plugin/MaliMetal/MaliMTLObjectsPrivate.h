/*
 * MaliMTLObjectsPrivate.h - declarations shared between the plug-in's Objective-C files
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliMTLObjectsPrivate.h
 * Not part of the plug-in's exported contract.
 */
#pragma once
#import "MaliMTLObjects.h"
#import "MaliShader.h"
#import "MaliResourceUsage.h"
#include "MaliPluginABI.h"

NS_ASSUME_NONNULL_BEGIN

/* ---- private IOGPU initialiser the queue uses (selector label verified on iOS 26.x, spec 04 D15) ---- */
@interface IOGPUMetalCommandQueue (MaliPrivate)
- (nullable instancetype)initWithDevice:(id)d descriptor:(MTLCommandQueueDescriptor *)desc
                                   args:(void *)args argsSize:(unsigned)sz;
@end

/* ====================================================================== *
 * Executable / descriptor memory shared by all pipelines of a device
 * ====================================================================== */
@interface MaliExecPool : NSObject
- (instancetype)initWithDevice:(id<MTLDevice>)device;
/* Carve `size` bytes (aligned to `align`) out of GPU-executable memory that lies in one 4 GiB window
 * (S07 6.13). Returns the CPU/GPU pointers; `holder` is the buffer the pipeline must keep alive and list in
 * the segment's resource list while it is used. */
- (BOOL)allocate:(uint32_t)size align:(uint32_t)align out:(mali_gpu_ptr_t *)out holder:(id<MTLBuffer> _Nullable *_Nonnull)holder;
@end

/* ====================================================================== *
 * Compiler variants (what the MTLCompiler service hands back, SnapMetal pattern)
 * ====================================================================== */
@interface MaliFunctionVariant : MTLFunctionVariant
@property (nonatomic, readonly) id compilerOutput;
@property (nonatomic, readonly) NSUInteger stage;                  /* MTLB TYPE: 0 vertex, 1 fragment, 2 kernel */
@property (nonatomic, readonly, nullable) MaliCompilerInput *input;
@property (nonatomic, readonly, nullable) NSString *inputError;
- (instancetype)initWithCompilerOutput:(id)o stage:(NSUInteger)s;
@end

/* ====================================================================== *
 * Pipeline states
 * ====================================================================== */
@interface MaliRenderPipelineState : _MTLRenderPipelineState <MTLRenderPipelineState>
- (const mali_gfx_pipeline_t *)maliPipe;
- (NSArray *)maliResources;                         /* buffers holding code/SPD/blend shaders */
- (const MaliConstSampler *)maliConstSamplers:(uint32_t *)count stage:(int)stage;   /* 0 vertex, 1 fragment */
@property (nonatomic, readonly) uint32_t maliVertexAttrMask;
@end

@interface MaliComputePipelineState : _MTLComputePipelineState <MTLComputePipelineState>
- (const mali_compute_pipeline_t *)maliPipe;
- (NSArray *)maliResources;
- (const MaliConstSampler *)maliConstSamplers:(uint32_t *)count;
/* The program variant that guards the body with all(gid < threads_per_grid) (dispatchThreads: with a
 * grid that is not a multiple of the threadgroup); compiled on first use. */
- (nullable const mali_compute_pipeline_t *)maliNonUniformPipe:(NSString *_Nullable *_Nullable)error;
- (NSArray *)maliNonUniformResources;
@end

/* Copy a finished program into GPU-executable memory and build its SPDs (MaliPipeline.m). `holders` collects the buffers
 * the pipeline must list in the segment's resource list. */
BOOL MaliUploadShader(MaliMTLDevice *dev, MaliShaderProgram *p, mali_shader_t *sh, NSMutableArray *holders, NSString *_Nullable *_Nullable err);

/* ====================================================================== *
 * Device internals
 * ====================================================================== */
@interface MaliMTLDevice (Pipeline)
- (int)llvmVersion;
@end

/* Built-in AIR kernels/shaders (MaliInternal.metal, embedded in the bundle's __MALI,__air section):
 * fullscreen "load" draw, buffer fill/copy, texture<->buffer copies, mip generation. */
@interface MaliInternalShaders : NSObject
- (instancetype)initWithDevice:(MaliMTLDevice *)device;
/* Render pipeline drawing a fullscreen triangle that copies texture(0) into color attachment `rt`
 * (LoadActionLoad). `kind`: 0 float-like, 1 uint, 2 sint. */
- (nullable const mali_gfx_pipeline_t *)loadPipelineForRT:(uint32_t)rt format:(uint32_t)mtlFormat
                                                    kind:(int)kind resources:(NSArray *_Nullable *_Nonnull)res;
/* A compute pipeline by built-in name ("fill_u32", "copy_buf", ...). */
- (nullable const mali_compute_pipeline_t *)computePipeline:(NSString *)name resources:(NSArray *_Nullable *_Nonnull)res;
@end

/* ====================================================================== *
 * Command buffer extension points used by the encoders
 * ====================================================================== */
@interface MaliCommandBuffer (Encoding)
- (void)maliHold:(id)object;                                                    /* keep alive until completion */
- (nullable NSString *)maliActiveEncoderKind;
- (void)maliNoteShadowWrite:(id)shadow;
@end

/* ====================================================================== *
 * Helpers in MaliResources.m
 * ====================================================================== */
/* The IOGPU resource that backs `resource` on the GPU (the texture itself, or the aligned shadow buffer of an IOSurface
 * texture); also refills a dirty shadow. */
id MaliGPUBacking(id resource);
/* GPU address of the first byte of the texture's level 0 (backing base + plane offset). */
uint64_t MaliTextureGPUBase(id<MTLTexture> t);
id _Nullable MaliShadowOfBacking(id resource);
void MaliShadowWriteBack(id shadow);

/* Thin accessors */
static inline MaliMTLDevice *MaliDeviceOf(id obj) { return (MaliMTLDevice *)[obj device]; }

NS_ASSUME_NONNULL_END
