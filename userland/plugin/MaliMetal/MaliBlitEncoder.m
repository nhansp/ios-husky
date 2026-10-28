/*
 * MaliBlitEncoder.m - blit command encoder
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliBlitEncoder.m
 *
 * Every blit operation runs as a built-in compute kernel (shaders/MaliInternal.metal) inside the command buffer's
 * own command stream, so it stays ordered against render passes and dispatches (MaliPluginABI.h: MALI_KCMD_FILL
 * exists in the contract but this plug-in does not use it). Textures are linear in v1 (mali_texture.c), which makes
 * buffer<->texture and texture<->texture copies strided memory copies: the kernels move bytes, no format
 * conversion, so source and destination must agree on bytes per pixel (compressed formats are refused).
 */
#import "MaliMTLObjectsPrivate.h"
#import "MaliBindings.h"

#define MALI_UNSUPPORTED(what) MaliLogOnce(@"blit encoder: " what " is not supported")

/* the kernels' parameter block (MaliCopyParams / MaliFillParams in MaliInternal.metal) */
struct copy_params { uint32_t row_units, rows, depth, src_row, src_img, dst_row, dst_img; };
struct fill_params { uint32_t count, value; };

/* A texture region as bytes in memory */
typedef struct tex_span {
    uint64_t gpu;                /* first byte of the region                        */
    uint32_t row, img;           /* strides in bytes                                */
    uint32_t row_bytes, rows, depth;
    uint32_t bpp;
    uint64_t span;               /* bytes from `gpu` to one past the last byte touched */
} tex_span_t;

@implementation MaliBlitCommandEncoder {
    MaliCommandBuffer *_cb;
    BOOL _ended, _began, _refused;
}
- (instancetype)initWithCommandBuffer:(id<MTLCommandBuffer>)cb {
    if (!(self = [super initWithCommandBuffer:cb])) return nil;
    _cb = (MaliCommandBuffer *)cb;
    @try { [_cb maliBeginEncoder:@"blit"]; _began = YES; }
    @catch (NSException *e) { [_cb maliFail:e.reason]; _refused = YES; }
    return self;
}
/* NO for a refused encoder: its command buffer has failed and every call is dropped */
- (BOOL)maliCheckOpen {
    if (_ended || !_cb) [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: blit encoder has ended"];
    return !_refused;
}
- (void)endEncoding {
    const BOOL open = [self maliCheckOpen];
    _ended = YES;
    @try { if (open || _began) [_cb maliEndEncoder]; } @finally { [super endEncoding]; }
}

/* ---- dispatch one built-in kernel ---- */
- (BOOL)maliRun:(NSString *)kernel binds:(mali_stage_binds_t *)b grid:(MTLSize)grid tg:(MTLSize)tg {
    MaliMTLDevice *dev = (MaliMTLDevice *)_cb.device;
    NSArray *holders = nil;
    const mali_compute_pipeline_t *pp = [[dev maliInternal] computePipeline:kernel resources:&holders];
    if (!pp) { MaliLogOnce([NSString stringWithFormat:@"blit: built-in kernel %@ unavailable", kernel]); return NO; }
    for (id h in holders) [_cb maliUseResource:h usage:MTLResourceUsageRead];
    mali_cs_t *cs = [_cb maliCS];
    if (!cs) { [_cb maliFail:@"command stream allocation failed"]; return NO; }
    mali_dispatch_t d;
    memset(&d, 0, sizeof d);
    d.pipe = pp;
    d.grid[0] = (uint32_t)grid.width; d.grid[1] = (uint32_t)grid.height; d.grid[2] = (uint32_t)grid.depth;
    d.tg_size[0] = (uint32_t)tg.width; d.tg_size[1] = (uint32_t)tg.height; d.tg_size[2] = (uint32_t)tg.depth;
    for (int i = 0; i < 3; i++) d.threads_per_grid[i] = d.grid[i] * d.tg_size[i];
    d.binds = b;
    d.desc_arena = [_cb maliArena];
    if (!mali_cs_dispatch(cs, [dev maliCtx], &d)) { [_cb maliFail:[@"blit kernel failed: " stringByAppendingString:kernel]]; return NO; }
    mali_cs_encoder_barrier(cs);
    return YES;
}

/* Strided copy src -> dst of `depth` images of `rows` rows of `row_bytes`. Strides in bytes. */
- (void)maliCopy:(uint64_t)src srcRow:(uint32_t)sr srcImg:(uint32_t)si srcSpan:(uint64_t)sspan
              to:(uint64_t)dst dstRow:(uint32_t)dr dstImg:(uint32_t)di dstSpan:(uint64_t)dspan
        rowBytes:(uint32_t)rb rows:(uint32_t)rows depth:(uint32_t)depth {
    if (!rb || !rows || !depth) return;
    mali_stage_binds_t b;
    MaliBindsInit(&b);
    b.buf_gpu[0] = src; b.buf_size[0] = sspan;
    b.buf_gpu[1] = dst; b.buf_size[1] = dspan;
    const BOOL words = !((src | dst | rb | sr | si | dr | di) & 3);
    const uint32_t u = words ? 4 : 1;
    struct copy_params p = {rb / u, rows, depth, sr / u, si / u, dr / u, di / u};
    mali_gpu_ptr_t m = mali_arena_alloc([_cb maliArena], sizeof p, 64);
    if (!m.cpu) { [_cb maliFail:@"descriptor arena exhausted (blit)"]; return; }
    memcpy(m.cpu, &p, sizeof p);
    b.buf_gpu[2] = m.gpu; b.buf_size[2] = sizeof p;
    const MTLSize tg = MTLSizeMake(32, 2, 1);
    const MTLSize grid = MTLSizeMake((p.row_units + 31) / 32, (rows + 1) / 2, depth);
    [self maliRun:words ? @"mali_copy2d_u32" : @"mali_copy2d_u8" binds:&b grid:grid tg:tg];
}

/* Where region (origin, size) of (level, slice) of `t` lives; NO (and a log line) if it cannot be copied as bytes. */
- (BOOL)maliSpan:(tex_span_t *)out ofTexture:(id<MTLTexture>)tex slice:(NSUInteger)slice level:(NSUInteger)level origin:(MTLOrigin)o size:(MTLSize)sz {
    MaliTexture *t = [tex isKindOfClass:[MaliTexture class]] ? (MaliTexture *)tex : nil;
    mali_texture_t *mt = [t maliTexture];
    mali_fmt_info_t fi;
    mali_subresource_t sr;
    if (!mt || !mali_fmt_lookup((uint32_t)tex.pixelFormat, &fi) || !fi.bytes_per_pixel || !(fi.flags & MALI_FMT_CPU_LINEAR) ||
        !mali_texture_subresource(mt, (uint32_t)level, (uint32_t)slice, &sr)) {
        MaliLogOnce([NSString stringWithFormat:@"blit: texture fmt %lu level %lu slice %lu cannot be copied as bytes", (unsigned long)tex.pixelFormat,
                     (unsigned long)level, (unsigned long)slice]);
        return NO;
    }
    if (o.x + sz.width > sr.width || o.y + sz.height > sr.height || o.z + sz.depth > MAX(sr.depth, 1u) || !sz.width || !sz.height || !sz.depth) {
        MaliLogOnce(@"blit: region outside the texture level");
        return NO;
    }
    const uint32_t img = sr.depth > 1 ? (uint32_t)sr.slice_stride_B : (uint32_t)(sr.row_stride_B * sr.height);
    out->gpu = mali_texture_base(mt) + sr.offset_B + o.z * img + o.y * sr.row_stride_B + o.x * fi.bytes_per_pixel;
    out->row = sr.row_stride_B;
    out->img = img;
    out->row_bytes = (uint32_t)(sz.width * fi.bytes_per_pixel);
    out->rows = (uint32_t)sz.height;
    out->depth = (uint32_t)sz.depth;
    out->bpp = fi.bytes_per_pixel;
    out->span = (uint64_t)(sz.depth - 1) * img + (uint64_t)(sz.height - 1) * sr.row_stride_B + out->row_bytes;
    return YES;
}

/* ---- buffer <-> buffer ---- */
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)dst destinationOffset:(NSUInteger)dof size:(NSUInteger)size {
    if (![self maliCheckOpen]) return;
    if (!size) return;
    if (so + size > src.length || dof + size > dst.length) { MaliLogOnce(@"blit: buffer copy out of range: dropped"); return; }
    [_cb maliUseResource:src usage:MTLResourceUsageRead];
    [_cb maliUseResource:dst usage:MTLResourceUsageWrite];
    const uint64_t s = [(IOGPUMetalBuffer *)src gpuAddress] + so, d = [(IOGPUMetalBuffer *)dst gpuAddress] + dof;
    /* one row: whole copy as a single strided copy of `size` bytes (the kernels pick words when everything is 4-aligned) */
    [self maliCopy:s srcRow:0 srcImg:0 srcSpan:size to:d dstRow:0 dstImg:0 dstSpan:size rowBytes:(uint32_t)size rows:1 depth:1];
}

/* ---- buffer -> texture ---- */
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)sbpr sourceBytesPerImage:(NSUInteger)sbpi
            sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
     destinationOrigin:(MTLOrigin)origin {
    if (![self maliCheckOpen]) return;
    tex_span_t t;
    if (![self maliSpan:&t ofTexture:dst slice:slice level:level origin:origin size:size]) return;
    const uint64_t span = (uint64_t)(size.depth - 1) * sbpi + (uint64_t)(size.height - 1) * sbpr + t.row_bytes;
    if (sbpr < t.row_bytes || so + span > src.length) { MaliLogOnce(@"blit: buffer->texture source out of range: dropped"); return; }
    [_cb maliUseResource:src usage:MTLResourceUsageRead];
    [_cb maliUseResource:dst usage:MTLResourceUsageWrite];
    [self maliCopy:[(IOGPUMetalBuffer *)src gpuAddress] + so srcRow:(uint32_t)sbpr srcImg:(uint32_t)(size.depth > 1 ? sbpi : sbpr * size.height) srcSpan:span
                to:t.gpu dstRow:t.row dstImg:t.img dstSpan:t.span rowBytes:t.row_bytes rows:t.rows depth:t.depth];
}
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)sbpr sourceBytesPerImage:(NSUInteger)sbpi
            sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level
     destinationOrigin:(MTLOrigin)origin options:(MTLBlitOption)opts {
    [self copyFromBuffer:src sourceOffset:so sourceBytesPerRow:sbpr sourceBytesPerImage:sbpi sourceSize:size toTexture:dst destinationSlice:slice
        destinationLevel:level destinationOrigin:origin];
}

/* ---- texture -> buffer ---- */
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin
             sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)dst destinationOffset:(NSUInteger)dof destinationBytesPerRow:(NSUInteger)dbpr
   destinationBytesPerImage:(NSUInteger)dbpi {
    if (![self maliCheckOpen]) return;
    tex_span_t t;
    if (![self maliSpan:&t ofTexture:src slice:slice level:level origin:origin size:size]) return;
    const uint64_t span = (uint64_t)(size.depth - 1) * dbpi + (uint64_t)(size.height - 1) * dbpr + t.row_bytes;
    if (dbpr < t.row_bytes || dof + span > dst.length) { MaliLogOnce(@"blit: texture->buffer destination out of range: dropped"); return; }
    [_cb maliUseResource:src usage:MTLResourceUsageRead];
    [_cb maliUseResource:dst usage:MTLResourceUsageWrite];
    [self maliCopy:t.gpu srcRow:t.row srcImg:t.img srcSpan:t.span
                to:[(IOGPUMetalBuffer *)dst gpuAddress] + dof dstRow:(uint32_t)dbpr dstImg:(uint32_t)(size.depth > 1 ? dbpi : dbpr * size.height) dstSpan:span
          rowBytes:t.row_bytes rows:t.rows depth:t.depth];
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level sourceOrigin:(MTLOrigin)origin
             sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)dst destinationOffset:(NSUInteger)dof destinationBytesPerRow:(NSUInteger)dbpr
   destinationBytesPerImage:(NSUInteger)dbpi options:(MTLBlitOption)opts {
    [self copyFromTexture:src sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size toBuffer:dst destinationOffset:dof
        destinationBytesPerRow:dbpr destinationBytesPerImage:dbpi];
}

/* ---- texture -> texture ---- */
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    if (![self maliCheckOpen]) return;
    tex_span_t a, b;
    if (![self maliSpan:&a ofTexture:src slice:ss level:sl origin:so size:size] || ![self maliSpan:&b ofTexture:dst slice:ds level:dl origin:dorg size:size]) return;
    if (a.bpp != b.bpp) { MaliLogOnce(@"blit: texture copy between formats of different pixel size: dropped"); return; }
    [_cb maliUseResource:src usage:MTLResourceUsageRead];
    [_cb maliUseResource:dst usage:MTLResourceUsageWrite];
    [self maliCopy:a.gpu srcRow:a.row srcImg:a.img srcSpan:a.span to:b.gpu dstRow:b.row dstImg:b.img dstSpan:b.span rowBytes:a.row_bytes rows:a.rows depth:a.depth];
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg
                options:(MTLBlitOption)opts {
    [self copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:size toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
}
- (void)copyFromTexture:(id<MTLTexture>)src toTexture:(id<MTLTexture>)dst {
    const NSUInteger slices = src.textureType == MTLTextureTypeCube || src.textureType == MTLTextureTypeCubeArray ? src.arrayLength * 6 : MAX(src.arrayLength, (NSUInteger)1);
    [self copyFromTexture:src sourceSlice:0 sourceLevel:0 toTexture:dst destinationSlice:0 destinationLevel:0 sliceCount:slices levelCount:src.mipmapLevelCount];
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl toTexture:(id<MTLTexture>)dst destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl sliceCount:(NSUInteger)sc levelCount:(NSUInteger)lc {
    for (NSUInteger l = 0; l < lc; l++)
        for (NSUInteger s = 0; s < sc; s++) {
            const NSUInteger w = MAX((NSUInteger)1, src.width >> (sl + l)), h = MAX((NSUInteger)1, src.height >> (sl + l)),
                             d = src.textureType == MTLTextureType3D ? MAX((NSUInteger)1, src.depth >> (sl + l)) : 1;
            [self copyFromTexture:src sourceSlice:ss + s sourceLevel:sl + l sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, d)
                        toTexture:dst destinationSlice:ds + s destinationLevel:dl + l destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
}

/* ---- fill ---- */
- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value {
    if (![self maliCheckOpen]) return;
    if (!range.length) return;
    if (range.location + range.length > buffer.length) { MaliLogOnce(@"blit: fill range out of range: dropped"); return; }
    [_cb maliUseResource:buffer usage:MTLResourceUsageWrite];
    mali_stage_binds_t b;
    MaliBindsInit(&b);
    const uint64_t gpu = [(IOGPUMetalBuffer *)buffer gpuAddress] + range.location;
    const BOOL words = !(gpu & 3) && !(range.length & 3);
    struct fill_params p = {(uint32_t)(words ? range.length / 4 : range.length), words ? value * 0x01010101u : value};
    mali_gpu_ptr_t m = mali_arena_alloc([_cb maliArena], sizeof p, 64);
    if (!m.cpu) { [_cb maliFail:@"descriptor arena exhausted (fill)"]; return; }
    memcpy(m.cpu, &p, sizeof p);
    b.buf_gpu[0] = gpu; b.buf_size[0] = range.length;
    b.buf_gpu[1] = m.gpu; b.buf_size[1] = sizeof p;
    [self maliRun:words ? @"mali_fill_u32" : @"mali_fill_u8" binds:&b grid:MTLSizeMake((p.count + 63) / 64, 1, 1) tg:MTLSizeMake(64, 1, 1)];
}

/* ---- mipmaps: 2x2 box filter, float-class colour formats ---- */
- (void)generateMipmapsForTexture:(id<MTLTexture>)tex {
    if (![self maliCheckOpen]) return;
    MaliTexture *t = [tex isKindOfClass:[MaliTexture class]] ? (MaliTexture *)tex : nil;
    mali_texture_t *mt = [t maliTexture];
    mali_fmt_info_t fi;
    if (!mt || !mali_fmt_lookup((uint32_t)tex.pixelFormat, &fi) || (fi.flags & (MALI_FMT_INTEGER | MALI_FMT_DEPTH | MALI_FMT_STENCIL | MALI_FMT_SRGB)) ||
        !(fi.flags & MALI_FMT_STORAGE) || !(fi.flags & MALI_FMT_SAMPLED) ||
        (tex.textureType != MTLTextureType2D && tex.textureType != MTLTextureType2DArray)) {
        MaliLogOnce([NSString stringWithFormat:@"blit: generateMipmaps unsupported for fmt %lu type %lu", (unsigned long)tex.pixelFormat, (unsigned long)tex.textureType]);
        return;
    }
    [_cb maliUseResource:tex usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    const NSUInteger slices = MAX(tex.arrayLength, (NSUInteger)1);
    for (NSUInteger lvl = 1; lvl < tex.mipmapLevelCount; lvl++)
        for (NSUInteger s = 0; s < slices; s++) {
            mali_texture_t *src = mali_texture_view(mt, (uint32_t)tex.pixelFormat, MALI_TEX_2D, (uint32_t)lvl - 1, 1, (uint32_t)s, 1);
            mali_texture_t *dst = mali_texture_view(mt, (uint32_t)tex.pixelFormat, MALI_TEX_2D, (uint32_t)lvl, 1, (uint32_t)s, 1);
            mali_stage_binds_t b;
            MaliBindsInit(&b);
            const BOOL ok = src && dst && mali_emit_sampled_texture(src, [_cb maliArena], b.tex_desc[0]) &&
                            mali_emit_storage_texture(dst, [_cb maliArena], b.stex_desc[1]);
            if (src) mali_texture_free(src);
            if (dst) mali_texture_free(dst);
            if (!ok) { MaliLogOnce(@"blit: mip level descriptors could not be built"); return; }
            b.tex_mask = 1;
            b.stex_mask = 2;
            const NSUInteger w = MAX((NSUInteger)1, tex.width >> lvl), h = MAX((NSUInteger)1, tex.height >> lvl);
            if (![self maliRun:@"mali_mip_box_f" binds:&b grid:MTLSizeMake((w + 7) / 8, (h + 7) / 8, 1) tg:MTLSizeMake(8, 8, 1)]) return;
        }
}

/* ---- no-ops: everything in a command buffer is ordered; CPU access goes through the kernel's clean/invalidate ---- */
- (void)synchronizeResource:(id<MTLResource>)r {}
- (void)synchronizeTexture:(id<MTLTexture>)t slice:(NSUInteger)s level:(NSUInteger)l {}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)t {}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)t slice:(NSUInteger)s level:(NSUInteger)l {}
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)t {}
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)t slice:(NSUInteger)s level:(NSUInteger)l {}
- (void)resetTextureAccessCounters:(id<MTLTexture>)t region:(MTLRegion)r mipLevel:(NSUInteger)l slice:(NSUInteger)s {}
- (void)getTextureAccessCounters:(id<MTLTexture>)t region:(MTLRegion)r mipLevel:(NSUInteger)l slice:(NSUInteger)s resetCounters:(BOOL)reset
                countersBuffer:(id<MTLBuffer>)b countersBufferOffset:(NSUInteger)o { MALI_UNSUPPORTED("texture access counters"); }
- (void)updateFence:(id<MTLFence>)f {}
- (void)waitForFence:(id<MTLFence>)f {}
- (void)resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)b withRange:(NSRange)r { MALI_UNSUPPORTED("indirect command buffers"); }
- (void)copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)s sourceRange:(NSRange)r destination:(id<MTLIndirectCommandBuffer>)d destinationIndex:(NSUInteger)i { MALI_UNSUPPORTED("indirect command buffers"); }
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel {
    return [super methodSignatureForSelector:sel] ?: MaliDroppedSelectorSignature([self class], sel);
}
- (void)forwardInvocation:(NSInvocation *)inv {
    if (![super methodSignatureForSelector:inv.selector]) return;
    [super forwardInvocation:inv];
}
@end
