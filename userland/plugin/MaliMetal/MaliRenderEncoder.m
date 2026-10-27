/*
 * MaliRenderEncoder.m - render command encoder
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliRenderEncoder.m
 *
 * One render encoder = one mali_pass_t of the command buffer's command stream (mali_cs.c: tiler context + FBD at
 * begin, one IDVS job per draw, FINISH_TILING .. FINISH_FRAGMENT at end). The encoder records Metal state and turns
 * it into a mali_draw_state_t at each draw; it implements the hot methods CoreAnimation binds as cached IMPs and the
 * fixed-function state of update_encoder_state (spec 05 section 5.3).
 *
 * Divergences from Metal that the command builder cannot express (all logged once, all dropped rather than
 * raised so a partially working compositor stays visible):
 *   - LoadActionLoad is a fullscreen "load" draw with the attachment bound as a texture (MaliInternal.metal);
 *     depth/stencil Load is treated as DontCare;
 *   - visibility result buffers, MSAA, layered rendering, indirect draws, tile functions: unsupported.
 */
#import "MaliMTLObjectsPrivate.h"
#import "MaliBindings.h"
#include <stdatomic.h>

#define MALI_UNSUPPORTED(what) MaliLogOnce(@"render encoder: " what " is not supported")

static int load_kind(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatR8Uint: case MTLPixelFormatR16Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatR32Uint:
        case MTLPixelFormatRG16Uint: case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRGBA16Uint:
        case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGB10A2Uint:
            return 1;
        case MTLPixelFormatR8Sint: case MTLPixelFormatR16Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatR32Sint:
        case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA8Sint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA16Sint:
        case MTLPixelFormatRGBA32Sint:
            return 2;
        default:
            return 0;
    }
}
static inline uint32_t level_dim(NSUInteger v, NSUInteger level) { return (uint32_t)MAX((NSUInteger)1, level < 32 ? v >> level : 0); }

@implementation MaliRenderCommandEncoder {
    MaliCommandBuffer *_cb;
    BOOL _ended;
    mali_pass_t *_pass;
    uint32_t _width, _height;
    MaliRenderPipelineState *_pipeline;
    MaliDepthStencilState *_dss;
    mali_stage_binds_t _vs, _fs;
    BOOL _constDirty;
    /* fixed-function state (Metal defaults) */
    MTLViewport _viewport; BOOL _viewportSet;
    MTLScissorRect _scissor; BOOL _scissorSet;
    MTLCullMode _cull;
    MTLWinding _winding;
    MTLTriangleFillMode _fill;
    MTLDepthClipMode _depthClip;
    float _bias, _slope, _clamp;
    float _blend[4];
    uint32_t _refFront, _refBack;
    MTLVisibilityResultMode _visibility;
    /* textures already emitted for a slot: rebinding the same object needs no new descriptor */
    id<MTLTexture> _fragTex[MALI_MAX_TEXTURES], _vertTex[MALI_MAX_TEXTURES];
    /* buffers bound per slot, so setXBufferOffset:atIndex: can recompute the address */
    id<MTLBuffer> _vbuf[MALI_MAX_BUFFERS], _fbuf[MALI_MAX_BUFFERS];
    /* first passes of the process, written to the diag file at endEncoding */
    NSMutableString *_diag;
    MaliRenderPipelineState *_diagPipe;
    NSUInteger _diagDraws;
}

static _Atomic unsigned gPassDiagCount;

- (instancetype)initWithCommandBuffer:(id<MTLCommandBuffer>)cb descriptor:(MTLRenderPassDescriptor *)rp {
    if (!(self = [super initWithCommandBuffer:cb descriptor:rp])) return nil;
    _cb = (MaliCommandBuffer *)cb;
    MaliBindsInit(&_vs);
    MaliBindsInit(&_fs);
    _winding = MTLWindingClockwise;
    @try {
        [_cb maliBeginEncoder:@"render"];
        [self maliBeginPass:rp];
    } @catch (NSException *e) {
        [_cb maliFail:e.reason];
        _ended = YES;
        [super endEncoding];
        @throw;
    }
    return self;
}

static void fill_attachment(mali_attachment_t *a, MTLRenderPassAttachmentDescriptor *d, BOOL color, NSString **refusal) {
    memset(a, 0, sizeof *a);
    MaliTexture *t = [d.texture isKindOfClass:[MaliTexture class]] ? (MaliTexture *)d.texture : nil;
    if (!d.texture) return;
    if (!t || ![t maliTexture]) { *refusal = @"an attachment texture has no Mali layout"; return; }
    if (d.texture.sampleCount > 1) { *refusal = @"multisample attachments are not supported"; return; }
    a->tex = [t maliTexture];
    a->level = (uint32_t)d.level;
    a->slice = (uint32_t)(d.texture.textureType == MTLTextureType3D ? d.depthPlane : d.slice);
    a->load = (uint32_t)d.loadAction;                      /* MTLLoadAction values == MALI_LOAD_* */
    switch (d.storeAction) {                               /* Store, StoreAndMultisampleResolve, Unknown keep the data */
        case MTLStoreActionStore: case MTLStoreActionStoreAndMultisampleResolve: case MTLStoreActionUnknown: a->store = MALI_STORE_STORE; break;
        default: a->store = MALI_STORE_DONTCARE;
    }
    (void)color;
}

- (void)maliBeginPass:(MTLRenderPassDescriptor *)rp {
    MaliMTLDevice *dev = (MaliMTLDevice *)_cb.device;
    MaliCommandQueue *queue = (MaliCommandQueue *)_cb.commandQueue;
    id<MTLBuffer> qstate = [queue maliQueueState];
    if (!qstate) [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: cannot allocate the queue state"];

    mali_pass_desc_t pd;
    memset(&pd, 0, sizeof pd);
    NSString *refusal = nil;
    uint32_t w = 0, h = 0;
    for (uint32_t i = 0; i < MALI_MAX_RTS; i++) {
        MTLRenderPassColorAttachmentDescriptor *ca = rp.colorAttachments[i];
        if (!ca.texture) continue;
        mali_attachment_t *a = &pd.color[i];
        fill_attachment(a, ca, YES, &refusal);
        const MTLClearColor c = ca.clearColor;
        a->clear_color[0] = (float)c.red; a->clear_color[1] = (float)c.green; a->clear_color[2] = (float)c.blue; a->clear_color[3] = (float)c.alpha;
        pd.color_count = i + 1;
        if (!w) { w = level_dim(ca.texture.width, ca.level); h = level_dim(ca.texture.height, ca.level); }
    }
    if (rp.depthAttachment.texture) {
        fill_attachment(&pd.depth, rp.depthAttachment, NO, &refusal);
        pd.depth.clear_depth = rp.depthAttachment.clearDepth;
        if (pd.depth.load == MALI_LOAD_LOAD) { MALI_UNSUPPORTED("depth LoadActionLoad (treated as DontCare)"); pd.depth.load = MALI_LOAD_DONTCARE; }
        if (!w) { w = level_dim(rp.depthAttachment.texture.width, rp.depthAttachment.level); h = level_dim(rp.depthAttachment.texture.height, rp.depthAttachment.level); }
    }
    if (rp.stencilAttachment.texture) {
        fill_attachment(&pd.stencil, rp.stencilAttachment, NO, &refusal);
        pd.stencil.clear_stencil = rp.stencilAttachment.clearStencil;
        if (pd.stencil.load == MALI_LOAD_LOAD) { MALI_UNSUPPORTED("stencil LoadActionLoad (treated as DontCare)"); pd.stencil.load = MALI_LOAD_DONTCARE; }
        if (!w) { w = level_dim(rp.stencilAttachment.texture.width, rp.stencilAttachment.level); h = level_dim(rp.stencilAttachment.texture.height, rp.stencilAttachment.level); }
    }
    if (rp.renderTargetWidth && rp.renderTargetWidth < w) w = (uint32_t)rp.renderTargetWidth;      /* a smaller render area */
    if (rp.renderTargetHeight && rp.renderTargetHeight < h) h = (uint32_t)rp.renderTargetHeight;
    if (rp.renderTargetArrayLength > 1) refusal = @"layered rendering is not supported";
    if (rp.visibilityResultBuffer) MALI_UNSUPPORTED("visibility result buffers (results stay zero)");
    if (refusal) [NSException raise:NSInvalidArgumentException format:@"MaliMetal: render pass refused: %@", refusal];
    if (!w || !h) [NSException raise:NSInvalidArgumentException format:@"MaliMetal: render pass without attachments"];

    /* LoadActionLoad is drawn below; the FBD itself preloads nothing */
    BOOL loads[MALI_MAX_RTS] = {NO};
    for (uint32_t i = 0; i < pd.color_count; i++)
        if (pd.color[i].tex && pd.color[i].load == MALI_LOAD_LOAD) { loads[i] = YES; pd.color[i].load = MALI_LOAD_DONTCARE; }

    pd.width = _width = w;
    pd.height = _height = h;
    pd.sample_count = 1;
    pd.layers = 1;
    pd.heap_desc_gpu = [(IOGPUMetalBuffer *)qstate gpuAddress] + MALI_QSTATE_HEAP_DESC_OFF;
    pd.desc_arena = [_cb maliArena];

    /* every attachment is read and written by the pass */
    for (uint32_t i = 0; i < pd.color_count; i++)
        if (rp.colorAttachments[i].texture) [_cb maliUseResource:rp.colorAttachments[i].texture usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    if (rp.depthAttachment.texture) [_cb maliUseResource:rp.depthAttachment.texture usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    if (rp.stencilAttachment.texture) [_cb maliUseResource:rp.stencilAttachment.texture usage:MTLResourceUsageRead | MTLResourceUsageWrite];

    mali_cs_t *cs = [_cb maliCS];
    if (!cs) [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: command stream allocation failed"];
    if (atomic_fetch_add(&gPassDiagCount, 1) < 48) {
        _diag = [NSMutableString stringWithFormat:@"pass %ux%u", w, h];
        for (uint32_t i = 0; i < pd.color_count; i++) {
            id<MTLTexture> t = rp.colorAttachments[i].texture;
            if (t) [_diag appendFormat:@" rt%u fmt%lu %lux%lu%@ load%u%@ store%u", i, (unsigned long)t.pixelFormat, (unsigned long)t.width,
                    (unsigned long)t.height, t.iosurface ? @" iosurface" : @"", (unsigned)rp.colorAttachments[i].loadAction,
                    loads[i] ? @"(draw)" : @"", pd.color[i].store];
        }
        if (rp.depthAttachment.texture) [_diag appendFormat:@" depth fmt%lu", (unsigned long)rp.depthAttachment.texture.pixelFormat];
        if (rp.stencilAttachment.texture) [_diag appendFormat:@" stencil fmt%lu", (unsigned long)rp.stencilAttachment.texture.pixelFormat];
    }
    _pass = mali_pass_begin(cs, [dev maliCtx], &pd);
    if (!_pass) [NSException raise:NSInvalidArgumentException format:@"MaliMetal: the command builder refused the render pass (%ux%u, %u colour attachments)", w, h, pd.color_count];
    _cb.maliUsedRender = YES;

    for (uint32_t i = 0; i < pd.color_count; i++)
        if (loads[i]) [self maliLoadDrawForAttachment:rp.colorAttachments[i] rt:i];
}

/* LoadActionLoad: draw the attachment's previous contents over the whole target */
- (void)maliLoadDrawForAttachment:(MTLRenderPassColorAttachmentDescriptor *)ca rt:(uint32_t)rt {
    MaliMTLDevice *dev = (MaliMTLDevice *)_cb.device;
    MaliTexture *t = (MaliTexture *)ca.texture;
    NSArray *holders = nil;
    const mali_gfx_pipeline_t *gp = [[dev maliInternal] loadPipelineForRT:rt format:(uint32_t)ca.texture.pixelFormat
                                                                    kind:load_kind(ca.texture.pixelFormat) resources:&holders];
    if (!gp) { MaliLogOnce(@"LoadActionLoad: built-in load pipeline unavailable; the attachment keeps undefined contents"); return; }
    for (id h in holders) [_cb maliUseResource:h usage:MTLResourceUsageRead];

    /* the attachment's level/slice as a 2D texture */
    mali_texture_t *view = mali_texture_view([t maliTexture], (uint32_t)ca.texture.pixelFormat, MALI_TEX_2D, (uint32_t)ca.level, 1,
                                             (uint32_t)(ca.texture.textureType == MTLTextureType3D ? ca.depthPlane : ca.slice), 1);
    if (!view) { MaliLogOnce(@"LoadActionLoad: no 2D view of the attachment"); return; }
    mali_stage_binds_t vs, fs;
    MaliBindsInit(&vs);
    MaliBindsInit(&fs);
    const BOOL ok = mali_emit_sampled_texture(view, [_cb maliArena], fs.tex_desc[0]);
    mali_texture_free(view);
    if (!ok) { MaliLogOnce(@"LoadActionLoad: no sampled descriptor for the attachment"); return; }
    fs.tex_mask = 1;

    mali_draw_state_t s;
    memset(&s, 0, sizeof s);
    s.pipe = gp;
    s.vs_binds = &vs;
    s.fs_binds = &fs;
    s.viewport.x = 0; s.viewport.y = 0; s.viewport.w = (float)_width; s.viewport.h = (float)_height; s.viewport.znear = 0; s.viewport.zfar = 1;
    s.viewport_set = true;
    s.prim = MTLPrimitiveTypeTriangle;
    s.count = 3;
    s.instance_count = 1;
    s.desc_arena = [_cb maliArena];
    if (!mali_pass_draw(_pass, &s)) [_cb maliFail:@"load draw failed"];
}

- (void)maliCheckOpen {
    if (_ended || !_cb) [NSException raise:NSInternalInconsistencyException format:@"MaliMetal: render encoder has ended"];
}
- (void)endEncoding {
    [self maliCheckOpen];
    mali_pass_t *pass = _pass;
    _pass = NULL;
    _ended = YES;
    @try {
        [_cb maliEndEncoder];
        if (_diag) MaliDiagLine([_diag stringByAppendingFormat:@" | draws %lu%@", (unsigned long)_diagDraws,
                                                                _cb.maliEncodingFailed ? @" FAILED" : @""]);
        _diag = nil;
        if (pass && !_cb.maliEncodingFailed) {
            if (!mali_pass_end(pass)) [_cb maliFail:@"mali_pass_end failed"];
            else mali_cs_encoder_barrier([_cb maliCS]);       /* the pass's writes are visible to whatever follows */
        } else if (pass) mali_pass_abort(pass);
    } @finally { [super endEncoding]; }
}

/* ---- resource usage ---- */
- (void)maliUse:(const id<MTLResource> [])rs count:(NSUInteger)n usage:(MTLResourceUsage)u {
    [self maliCheckOpen];
    for (NSUInteger i = 0; i < n; i++) [_cb maliUseResource:rs[i] usage:u];
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u { [self maliUse:&r count:1 usage:u]; }
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)s { [self maliUse:&r count:1 usage:u]; }
- (void)useResources:(const id<MTLResource> [])rs count:(NSUInteger)n usage:(MTLResourceUsage)u { [self maliUse:rs count:n usage:u]; }
- (void)useResources:(const id<MTLResource> [])rs count:(NSUInteger)n usage:(MTLResourceUsage)u stages:(MTLRenderStages)s { [self maliUse:rs count:n usage:u]; }
- (void)useHeap:(id<MTLHeap>)h { MALI_UNSUPPORTED("useHeap"); }
- (void)useHeap:(id<MTLHeap>)h stages:(MTLRenderStages)s { MALI_UNSUPPORTED("useHeap"); }
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope afterStages:(MTLRenderStages)a beforeStages:(MTLRenderStages)b {}
- (void)memoryBarrierWithResources:(const id<MTLResource> [])r count:(NSUInteger)n afterStages:(MTLRenderStages)a beforeStages:(MTLRenderStages)b {}
- (void)textureBarrier {}

/* ---- pipeline and fixed-function state ---- */
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)s {
    [self maliCheckOpen];
    if (s && ![s isKindOfClass:[MaliRenderPipelineState class]]) { MaliLogOnce(@"setRenderPipelineState: not a MaliMetal pipeline"); _pipeline = nil; return; }
    _pipeline = (MaliRenderPipelineState *)s;
    for (id h in [_pipeline maliResources]) [_cb maliUseResource:h usage:MTLResourceUsageRead];
    _constDirty = YES;
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s { [self maliCheckOpen]; _dss = [s isKindOfClass:[MaliDepthStencilState class]] ? (MaliDepthStencilState *)s : nil; }
- (void)setViewport:(MTLViewport)v { [self maliCheckOpen]; _viewport = v; _viewportSet = YES; }
- (void)setViewports:(const MTLViewport *)v count:(NSUInteger)n { if (n) [self setViewport:v[0]]; }
- (void)setScissorRect:(MTLScissorRect)r { [self maliCheckOpen]; _scissor = r; _scissorSet = YES; }
- (void)setScissorRects:(const MTLScissorRect *)r count:(NSUInteger)n { if (n) [self setScissorRect:r[0]]; }
- (void)setCullMode:(MTLCullMode)m { _cull = m; }
- (void)setFrontFacingWinding:(MTLWinding)w { _winding = w; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { _fill = m; }
- (void)setDepthClipMode:(MTLDepthClipMode)m { _depthClip = m; }
- (void)setDepthBias:(float)b slopeScale:(float)s clamp:(float)c { _bias = b; _slope = s; _clamp = c; }
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a { _blend[0] = r; _blend[1] = g; _blend[2] = b; _blend[3] = a; }
- (void)setStencilReferenceValue:(uint32_t)v { _refFront = _refBack = v; }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b { _refFront = f; _refBack = b; }
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)m offset:(NSUInteger)o { _visibility = m; if (m != MTLVisibilityResultModeDisabled) MALI_UNSUPPORTED("visibility results"); }
- (void)setDepthTestMinBound:(float)a maxBound:(float)b { MALI_UNSUPPORTED("depth bounds"); }
- (void)setColorStoreAction:(MTLStoreAction)a atIndex:(NSUInteger)i { MALI_UNSUPPORTED("setColorStoreAction"); }
- (void)setDepthStoreAction:(MTLStoreAction)a { MALI_UNSUPPORTED("setDepthStoreAction"); }
- (void)setStencilStoreAction:(MTLStoreAction)a { MALI_UNSUPPORTED("setStencilStoreAction"); }
- (void)setFineGrainedBackgroundVisibilityEnabled:(BOOL)e {}     /* CoreAnimation SPI: nothing to tell */
- (void)setLevel:(NSUInteger)l {}

/* ---- bindings ---- */
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self maliCheckOpen]; if (i < MALI_MAX_BUFFERS) _vbuf[i] = b; MaliBindBuffer(&_vs, _cb, i, b, o); }
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < MALI_MAX_BUFFERS) MaliBindBufferOffset(&_vs, i, o, _vbuf[i]); }
- (void)setVertexBytes:(const void *)b length:(NSUInteger)l atIndex:(NSUInteger)i { [self maliCheckOpen]; if (i < MALI_MAX_BUFFERS) _vbuf[i] = nil; MaliBindBytes(&_vs, _cb, i, b, l); }
- (void)setVertexBuffers:(const id<MTLBuffer> [])bs offsets:(const NSUInteger [])os withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setVertexBuffer:bs[k] offset:os[k] atIndex:r.location + k];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self maliCheckOpen]; if (i < MALI_MAX_BUFFERS) _fbuf[i] = b; MaliBindBuffer(&_fs, _cb, i, b, o); }
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { if (i < MALI_MAX_BUFFERS) MaliBindBufferOffset(&_fs, i, o, _fbuf[i]); }
- (void)setFragmentBytes:(const void *)b length:(NSUInteger)l atIndex:(NSUInteger)i { [self maliCheckOpen]; if (i < MALI_MAX_BUFFERS) _fbuf[i] = nil; MaliBindBytes(&_fs, _cb, i, b, l); }
- (void)setFragmentBuffers:(const id<MTLBuffer> [])bs offsets:(const NSUInteger [])os withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setFragmentBuffer:bs[k] offset:os[k] atIndex:r.location + k];
}
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {
    [self maliCheckOpen];
    if (i < MALI_MAX_TEXTURES) { if (_fragTex[i] == t && t) return; _fragTex[i] = t; }
    MaliBindTexture(&_fs, _cb, i, t);
}
- (void)setFragmentTextures:(const id<MTLTexture> [])ts withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setFragmentTexture:ts[k] atIndex:r.location + k]; }
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {
    [self maliCheckOpen];
    if (i < MALI_MAX_TEXTURES) { if (_vertTex[i] == t && t) return; _vertTex[i] = t; }
    MaliBindTexture(&_vs, _cb, i, t);
}
- (void)setVertexTextures:(const id<MTLTexture> [])ts withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setVertexTexture:ts[k] atIndex:r.location + k]; }
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { [self maliCheckOpen]; MaliBindSampler(&_fs, i, s); _constDirty = YES; }
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { [self setFragmentSamplerState:s atIndex:i]; }
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> [])ss withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setFragmentSamplerState:ss[k] atIndex:r.location + k]; }
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { [self maliCheckOpen]; MaliBindSampler(&_vs, i, s); _constDirty = YES; }
- (void)setVertexSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)a lodMaxClamp:(float)b atIndex:(NSUInteger)i { [self setVertexSamplerState:s atIndex:i]; }
- (void)setVertexSamplerStates:(const id<MTLSamplerState> [])ss withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setVertexSamplerState:ss[k] atIndex:r.location + k]; }
- (void)setTileBytes:(const void *)b length:(NSUInteger)l atIndex:(NSUInteger)i { MALI_UNSUPPORTED("tile bytes"); }

/* ---- draws ---- */
- (void)maliDraw:(BOOL)indexed prim:(MTLPrimitiveType)prim start:(NSUInteger)start count:(NSUInteger)count instances:(NSUInteger)inst
      baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi indexType:(MTLIndexType)it indexBuffer:(id<MTLBuffer>)ib indexOffset:(NSUInteger)ioff {
    [self maliCheckOpen];
    if (!_pass || !_pipeline) { MaliLogOnce(@"draw without a pass or a (Mali) pipeline state: dropped"); return; }
    if (!count || !inst) return;
    if (_constDirty) {
        uint32_t n = 0;
        const MaliConstSampler *cs = [_pipeline maliConstSamplers:&n stage:0];
        if (n) MaliApplyConstSamplers(&_vs, cs, n);
        cs = [_pipeline maliConstSamplers:&n stage:1];
        if (n) MaliApplyConstSamplers(&_fs, cs, n);
        _constDirty = NO;
    }
    mali_draw_state_t s;
    memset(&s, 0, sizeof s);
    s.pipe = [_pipeline maliPipe];
    s.vs_binds = &_vs;
    s.fs_binds = &_fs;
    if (_viewportSet) {
        s.viewport.x = (float)_viewport.originX; s.viewport.y = (float)_viewport.originY;
        s.viewport.w = (float)_viewport.width; s.viewport.h = (float)_viewport.height;
        s.viewport.znear = (float)_viewport.znear; s.viewport.zfar = (float)_viewport.zfar;
    } else {                                                  /* Metal's default viewport is the whole render target */
        s.viewport.w = (float)_width; s.viewport.h = (float)_height; s.viewport.znear = 0; s.viewport.zfar = 1;
    }
    s.viewport_set = true;
    if (_scissorSet) { s.scissor.x = (uint32_t)_scissor.x; s.scissor.y = (uint32_t)_scissor.y; s.scissor.w = (uint32_t)_scissor.width; s.scissor.h = (uint32_t)_scissor.height; s.scissor_set = true; }
    s.cull_mode = (uint32_t)_cull;
    s.winding = (uint32_t)_winding;
    s.fill_mode = (uint32_t)_fill;
    s.depth_clip_mode = (uint32_t)_depthClip;
    s.depth_bias = _bias; s.depth_slope = _slope; s.depth_clamp = _clamp;
    memcpy(s.blend_color, _blend, sizeof _blend);
    s.stencil_ref_front = _refFront; s.stencil_ref_back = _refBack;
    s.zs = _dss ? [_dss maliZS] : NULL;
    s.visibility_mode = (uint32_t)_visibility;
    s.prim = (uint32_t)prim;
    s.count = (uint32_t)count;
    s.instance_count = (uint32_t)inst;
    s.base_instance = (uint32_t)bi;
    s.desc_arena = [_cb maliArena];
    if (indexed) {
        if (!ib || ioff > ib.length) { MaliLogOnce(@"indexed draw without a valid index buffer: dropped"); return; }
        s.indexed = true;
        s.index_type = (uint32_t)it;
        s.index_gpu = [(IOGPUMetalBuffer *)ib gpuAddress] + ioff;
        s.index_buffer_size = (uint32_t)MIN((NSUInteger)UINT32_MAX, ib.length - ioff);
        s.base_vertex = (int32_t)bv;
        [_cb maliUseResource:ib usage:MTLResourceUsageRead];
    } else s.vertex_start = (uint32_t)start;
    if (_diag) {
        _diagDraws++;
        if (_pipeline != _diagPipe) {
            _diagPipe = _pipeline;
            [_diag appendFormat:@" | %@ prim%u n%u i%u%@%@ fs_tex%x", [_pipeline maliDiagName], s.prim, s.count, s.instance_count,
                s.indexed ? @" idx" : @"", _scissorSet ? [NSString stringWithFormat:@" sc%ux%u", s.scissor.w, s.scissor.h] : @"",
                _fs.tex_mask];
        }
    }
    if (!mali_pass_draw(_pass, &s)) [_cb maliFail:@"mali_pass_draw failed"];
}
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c {
    [self maliDraw:NO prim:p start:s count:c instances:1 baseVertex:0 baseInstance:0 indexType:0 indexBuffer:nil indexOffset:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)n {
    [self maliDraw:NO prim:p start:s count:c instances:n baseVertex:0 baseInstance:0 indexType:0 indexBuffer:nil indexOffset:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)n baseInstance:(NSUInteger)bi {
    [self maliDraw:NO prim:p start:s count:c instances:n baseVertex:0 baseInstance:bi indexType:0 indexBuffer:nil indexOffset:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)c indexType:(MTLIndexType)t indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o {
    [self maliDraw:YES prim:p start:0 count:c instances:1 baseVertex:0 baseInstance:0 indexType:t indexBuffer:b indexOffset:o];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)c indexType:(MTLIndexType)t indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o instanceCount:(NSUInteger)n {
    [self maliDraw:YES prim:p start:0 count:c instances:n baseVertex:0 baseInstance:0 indexType:t indexBuffer:b indexOffset:o];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)c indexType:(MTLIndexType)t indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)n baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi {
    [self maliDraw:YES prim:p start:0 count:c instances:n baseVertex:bv baseInstance:bi indexType:t indexBuffer:b indexOffset:o];
}
- (void)drawPrimitives:(MTLPrimitiveType)p indirectBuffer:(id<MTLBuffer>)b indirectBufferOffset:(NSUInteger)o { MALI_UNSUPPORTED("indirect draws"); }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexType:(MTLIndexType)t indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)io
               indirectBuffer:(id<MTLBuffer>)b indirectBufferOffset:(NSUInteger)o { MALI_UNSUPPORTED("indirect indexed draws"); }
- (void)drawPatches:(NSUInteger)n patchStart:(NSUInteger)s patchCount:(NSUInteger)c patchIndexBuffer:(id<MTLBuffer>)pb patchIndexBufferOffset:(NSUInteger)po
      instanceCount:(NSUInteger)i baseInstance:(NSUInteger)bi { MALI_UNSUPPORTED("tessellation"); }
- (void)setTessellationFactorBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o instanceStride:(NSUInteger)s { MALI_UNSUPPORTED("tessellation"); }
- (void)setTessellationFactorScale:(float)f { MALI_UNSUPPORTED("tessellation"); }
- (void)setThreadgroupMemoryLength:(NSUInteger)l offset:(NSUInteger)o atIndex:(NSUInteger)i { MALI_UNSUPPORTED("tile threadgroup memory"); }
- (void)setTileBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { MALI_UNSUPPORTED("tile buffers"); }
- (void)dispatchThreadsPerTile:(MTLSize)s { MALI_UNSUPPORTED("tile dispatch"); }
@end
