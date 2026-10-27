#pragma once
#import <Foundation/Foundation.h>
#define NS_ENUM(t, n) enum n : t n; enum n : t
#define NS_OPTIONS(t, n) enum n : t n; enum n : t
typedef NS_OPTIONS(NSUInteger, MTLResourceOptions) {
    MTLResourceCPUCacheModeWriteCombined = 1, MTLResourceStorageModeShift = 4, MTLResourceStorageModeShared = 0, MTLResourceStorageModePrivate = 2 << 4,
};
typedef NS_ENUM(NSUInteger, MTLStorageMode) { MTLStorageModeShared = 0, MTLStorageModeManaged = 1, MTLStorageModePrivate = 2, MTLStorageModeMemoryless = 3 };
typedef NS_OPTIONS(NSUInteger, MTLResourceUsage) { MTLResourceUsageRead = 1, MTLResourceUsageWrite = 2 };
typedef NS_OPTIONS(NSUInteger, MTLTextureUsage) { MTLTextureUsageShaderRead = 1, MTLTextureUsageShaderWrite = 2, MTLTextureUsageRenderTarget = 4 };
typedef NS_ENUM(NSUInteger, MTLTextureType) { MTLTextureType1D, MTLTextureType1DArray, MTLTextureType2D, MTLTextureType2DArray, MTLTextureType2DMultisample, MTLTextureTypeCube, MTLTextureTypeCubeArray, MTLTextureType3D, MTLTextureType2DMultisampleArray, MTLTextureTypeTextureBuffer };
typedef NS_ENUM(NSUInteger, MTLPixelFormat) { MTLPixelFormatInvalid = 0,
 MTLPixelFormatR8Uint = 13, MTLPixelFormatR8Sint = 14, MTLPixelFormatR16Uint = 23, MTLPixelFormatR16Sint = 24, MTLPixelFormatRG8Uint = 33, MTLPixelFormatRG8Sint = 34,
 MTLPixelFormatR32Uint = 53, MTLPixelFormatR32Sint = 54, MTLPixelFormatRG16Uint = 63, MTLPixelFormatRG16Sint = 64, MTLPixelFormatRGBA8Uint = 73, MTLPixelFormatRGBA8Sint = 74,
 MTLPixelFormatRGB10A2Uint = 91, MTLPixelFormatRG32Uint = 103, MTLPixelFormatRG32Sint = 104, MTLPixelFormatRGBA16Uint = 113, MTLPixelFormatRGBA16Sint = 114,
 MTLPixelFormatRGBA32Uint = 123, MTLPixelFormatRGBA32Sint = 124 };
typedef NS_ENUM(NSUInteger, MTLVertexFormat) { MTLVertexFormatInvalid = 0 };
typedef NS_ENUM(NSInteger, MTLGPUFamily) { MTLGPUFamilyCommon1 = 3001, MTLGPUFamilyCommon2, MTLGPUFamilyCommon3, MTLGPUFamilyMac2 = 2002 };
typedef NS_ENUM(NSInteger, MTLArgumentBuffersTier) { MTLArgumentBuffersTier1 = 0, MTLArgumentBuffersTier2 = 1 };
typedef NS_ENUM(NSUInteger, MTLCompareFunction) { MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual, MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways };
typedef NS_ENUM(NSUInteger, MTLLoadAction) { MTLLoadActionDontCare, MTLLoadActionLoad, MTLLoadActionClear };
typedef NS_ENUM(NSUInteger, MTLStoreAction) { MTLStoreActionDontCare, MTLStoreActionStore, MTLStoreActionMultisampleResolve, MTLStoreActionStoreAndMultisampleResolve, MTLStoreActionUnknown };
typedef NS_ENUM(NSUInteger, MTLPrimitiveType) { MTLPrimitiveTypePoint, MTLPrimitiveTypeLine, MTLPrimitiveTypeLineStrip, MTLPrimitiveTypeTriangle, MTLPrimitiveTypeTriangleStrip };
typedef NS_ENUM(NSUInteger, MTLIndexType) { MTLIndexTypeUInt16, MTLIndexTypeUInt32 };
typedef NS_ENUM(NSUInteger, MTLCullMode) { MTLCullModeNone };
typedef NS_ENUM(NSUInteger, MTLWinding) { MTLWindingClockwise, MTLWindingCounterClockwise };
typedef NS_ENUM(NSUInteger, MTLTriangleFillMode) { MTLTriangleFillModeFill };
typedef NS_ENUM(NSUInteger, MTLDepthClipMode) { MTLDepthClipModeClip };
typedef NS_ENUM(NSUInteger, MTLVisibilityResultMode) { MTLVisibilityResultModeDisabled };
typedef NS_OPTIONS(NSUInteger, MTLBlitOption) { MTLBlitOptionNone };
typedef NS_OPTIONS(NSUInteger, MTLBarrierScope) { MTLBarrierScopeBuffers };
typedef NS_OPTIONS(NSUInteger, MTLRenderStages) { MTLRenderStageVertex };
typedef NS_ENUM(NSUInteger, MTLDispatchType) { MTLDispatchTypeSerial };
typedef NS_ENUM(NSInteger, MTLSparsePageSize) { MTLSparsePageSize16 };
typedef NS_ENUM(NSUInteger, MTLCommandBufferStatus) { MTLCommandBufferStatusNotEnqueued, MTLCommandBufferStatusEnqueued, MTLCommandBufferStatusCommitted, MTLCommandBufferStatusScheduled, MTLCommandBufferStatusCompleted, MTLCommandBufferStatusError };
typedef struct { NSUInteger width, height, depth; } MTLSize;
typedef struct { NSUInteger x, y, z; } MTLOrigin;
typedef struct { MTLOrigin origin; MTLSize size; } MTLRegion;
typedef struct { double originX, originY, width, height, znear, zfar; } MTLViewport;
typedef struct { NSUInteger x, y, width, height; } MTLScissorRect;
typedef struct { double red, green, blue, alpha; } MTLClearColor;
typedef struct { NSUInteger size, align; } MTLSizeAndAlign;
static inline MTLSize MTLSizeMake(NSUInteger w, NSUInteger h, NSUInteger d) { return (MTLSize){w, h, d}; }
static inline MTLOrigin MTLOriginMake(NSUInteger x, NSUInteger y, NSUInteger z) { return (MTLOrigin){x, y, z}; }
@protocol MTLResource, MTLTexture, MTLBuffer, MTLCommandBuffer, MTLDevice, MTLSamplerState, MTLDepthStencilState, MTLRenderPipelineState, MTLComputePipelineState, MTLHeap, MTLFence, MTLIndirectCommandBuffer, MTLCommandQueue, MTLBinaryArchive, MTLParallelRenderCommandEncoder, MTLResourceStateCommandEncoder;
typedef NS_ENUM(NSUInteger, MTLSamplerMinMagFilter) { MTLSamplerMinMagFilterNearest };
typedef NS_ENUM(NSUInteger, MTLSamplerMipFilter) { MTLSamplerMipFilterNotMipmapped };
typedef NS_ENUM(NSUInteger, MTLSamplerAddressMode) { MTLSamplerAddressModeClampToEdge };
typedef NS_ENUM(NSUInteger, MTLSamplerBorderColor) { MTLSamplerBorderColorTransparentBlack };
typedef NS_ENUM(NSUInteger, MTLStencilOperation) { MTLStencilOperationKeep };
typedef NS_ENUM(NSUInteger, MTLBlendFactor) { MTLBlendFactorZero };
typedef NS_ENUM(NSUInteger, MTLBlendOperation) { MTLBlendOperationAdd };
typedef NS_ENUM(NSUInteger, MTLColorWriteMask) { MTLColorWriteMaskAll };
typedef NS_ENUM(NSUInteger, MTLPrimitiveTopologyClass) { MTLPrimitiveTopologyClassUnspecified };
typedef NS_ENUM(NSUInteger, MTLVertexStepFunction) { MTLVertexStepFunctionConstant };
@interface MTLTextureDescriptor : NSObject
@property NSUInteger width, height, depth, arrayLength, mipmapLevelCount, sampleCount, resourceOptions; @property MTLTextureType textureType; @property MTLPixelFormat pixelFormat; @property MTLTextureUsage usage; @property MTLStorageMode storageMode;
@end
@interface MTLSamplerDescriptor : NSObject
@property MTLSamplerMinMagFilter minFilter, magFilter; @property MTLSamplerMipFilter mipFilter; @property MTLSamplerAddressMode sAddressMode, tAddressMode, rAddressMode; @property MTLSamplerBorderColor borderColor;
@property NSUInteger maxAnisotropy; @property MTLCompareFunction compareFunction; @property BOOL normalizedCoordinates; @property float lodMinClamp, lodMaxClamp;
@end
@interface MTLStencilDescriptor : NSObject
@property MTLCompareFunction stencilCompareFunction; @property MTLStencilOperation stencilFailureOperation, depthFailureOperation, depthStencilPassOperation; @property uint32_t readMask, writeMask;
@end
@interface MTLDepthStencilDescriptor : NSObject <NSCopying>
@property MTLCompareFunction depthCompareFunction; @property BOOL depthWriteEnabled; @property (copy) MTLStencilDescriptor *frontFaceStencil, *backFaceStencil;
@end
@interface MTLRenderPassAttachmentDescriptor : NSObject
@property (strong) id<MTLTexture> texture; @property NSUInteger level, slice, depthPlane; @property MTLLoadAction loadAction; @property MTLStoreAction storeAction; @property MTLTextureType textureType;
@end
@interface MTLRenderPassColorAttachmentDescriptor : MTLRenderPassAttachmentDescriptor @property MTLClearColor clearColor; @end
@interface MTLRenderPassDepthAttachmentDescriptor : MTLRenderPassAttachmentDescriptor @property double clearDepth; @end
@interface MTLRenderPassStencilAttachmentDescriptor : MTLRenderPassAttachmentDescriptor @property uint32_t clearStencil; @end
@interface MTLRenderPassColorAttachmentDescriptorArray : NSObject - (MTLRenderPassColorAttachmentDescriptor *)objectAtIndexedSubscript:(NSUInteger)i; @end
@interface MTLRenderPassDescriptor : NSObject
@property (readonly) MTLRenderPassColorAttachmentDescriptorArray *colorAttachments; @property (copy) MTLRenderPassDepthAttachmentDescriptor *depthAttachment; @property (copy) MTLRenderPassStencilAttachmentDescriptor *stencilAttachment;
@property NSUInteger renderTargetWidth, renderTargetHeight, renderTargetArrayLength; @property (strong) id<MTLBuffer> visibilityResultBuffer;
@end
@interface MTLVertexAttributeDescriptor : NSObject @property MTLVertexFormat format; @property NSUInteger offset, bufferIndex; @end
@interface MTLVertexBufferLayoutDescriptor : NSObject @property NSUInteger stride, stepRate; @property MTLVertexStepFunction stepFunction; @end
@interface MTLVertexAttributeDescriptorArray : NSObject - (MTLVertexAttributeDescriptor *)objectAtIndexedSubscript:(NSUInteger)i; @end
@interface MTLVertexBufferLayoutDescriptorArray : NSObject - (MTLVertexBufferLayoutDescriptor *)objectAtIndexedSubscript:(NSUInteger)i; @end
@interface MTLVertexDescriptor : NSObject @property (readonly) MTLVertexAttributeDescriptorArray *attributes; @property (readonly) MTLVertexBufferLayoutDescriptorArray *layouts; @end
@interface MTLRenderPipelineColorAttachmentDescriptor : NSObject
@property MTLPixelFormat pixelFormat; @property (getter=isBlendingEnabled) BOOL blendingEnabled; @property MTLBlendFactor sourceRGBBlendFactor, destinationRGBBlendFactor, sourceAlphaBlendFactor, destinationAlphaBlendFactor;
@property MTLBlendOperation rgbBlendOperation, alphaBlendOperation; @property MTLColorWriteMask writeMask;
@end
@interface MTLRenderPipelineColorAttachmentDescriptorArray : NSObject - (MTLRenderPipelineColorAttachmentDescriptor *)objectAtIndexedSubscript:(NSUInteger)i; @end
@protocol MTLFunction <NSObject> @property (readonly) NSString *name; @end
@interface MTLRenderPipelineDescriptor : NSObject
@property (strong) id<MTLFunction> vertexFunction, fragmentFunction; @property (copy) MTLVertexDescriptor *vertexDescriptor; @property (readonly) MTLRenderPipelineColorAttachmentDescriptorArray *colorAttachments;
@property MTLPixelFormat depthAttachmentPixelFormat, stencilAttachmentPixelFormat; @property NSUInteger rasterSampleCount, sampleCount; @property (getter=isAlphaToCoverageEnabled) BOOL alphaToCoverageEnabled; @property (getter=isAlphaToOneEnabled) BOOL alphaToOneEnabled;
@property (getter=isRasterizationEnabled) BOOL rasterizationEnabled; @property MTLPrimitiveTopologyClass inputPrimitiveTopology;
@end
@interface MTLComputePipelineDescriptor : NSObject @property (strong) id<MTLFunction> computeFunction; @end
@interface MTLCommandQueueDescriptor : NSObject @property NSUInteger maxCommandBufferCount; @end
@interface MTLCommandBufferDescriptor : NSObject @property BOOL retainedReferences; @end
@interface MTLBlitPassDescriptor : NSObject @end
@interface MTLComputePassDescriptor : NSObject @end
@interface MTLHeapDescriptor : NSObject @end
@interface MTLBinaryArchiveDescriptor : NSObject @end
@protocol MTLResource <NSObject> @property (readonly) id<MTLDevice> device; @property (copy) NSString *label; @end
@protocol MTLBuffer <MTLResource> @property (readonly) NSUInteger length; - (void *)contents; @end
@protocol MTLTexture <MTLResource>
@property (readonly) id<MTLBuffer> buffer; @property (readonly) NSUInteger bufferOffset, bufferBytesPerRow;
@property (readonly) NSUInteger width, height, depth, arrayLength, mipmapLevelCount, sampleCount; @property (readonly) MTLTextureType textureType; @property (readonly) MTLPixelFormat pixelFormat; @property (readonly) MTLTextureUsage usage;
@end
@protocol MTLSamplerState <NSObject> @end
@protocol MTLDepthStencilState <NSObject> @end
@protocol MTLRenderPipelineState <NSObject> @property (readonly) id<MTLDevice> device; @end
@protocol MTLComputePipelineState <NSObject> @property (readonly) id<MTLDevice> device; @property (readonly) NSUInteger maxTotalThreadsPerThreadgroup, threadExecutionWidth; @end
@protocol MTLHeap <NSObject> @end
@protocol MTLFence <NSObject> @end
@protocol MTLIndirectCommandBuffer <NSObject> @end
@protocol MTLBinaryArchive <NSObject> @end
@protocol MTLParallelRenderCommandEncoder <NSObject> @end
@protocol MTLResourceStateCommandEncoder <NSObject> @end
typedef void (^MTLCommandBufferHandler)(id<MTLCommandBuffer>);
@protocol MTLCommandEncoder <NSObject> - (void)endEncoding; @end
@protocol MTLRenderCommandEncoder <MTLCommandEncoder> @end
@protocol MTLBlitCommandEncoder <MTLCommandEncoder> @end
@protocol MTLComputeCommandEncoder <MTLCommandEncoder> @end
@protocol MTLCommandBuffer <NSObject>
@property (readonly) id<MTLDevice> device; @property (readonly) id<MTLCommandQueue> commandQueue; @property (readonly) BOOL retainedReferences; @property (readonly) MTLCommandBufferStatus status;
- (void)commit; - (void)waitUntilCompleted; - (void)addCompletedHandler:(MTLCommandBufferHandler)b;
@end
@protocol MTLCommandQueue <NSObject> @property (readonly) id<MTLDevice> device; @end
@protocol MTLDevice <NSObject> - (id<MTLBuffer>)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o; @end
