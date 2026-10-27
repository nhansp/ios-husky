/*
 * MaliPipeline.m - pipeline states, compiler variants and executable memory
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/MaliPipeline.m
 *
 * _MTLDevice's public pipeline entry points (newRenderPipelineStateWithDescriptor:error:, ...:options:reflection:error:,
 * newComputePipelineStateWithDescriptor:...) live in Metal. They run the MTLCompiler returned by -compiler (XPC to
 * MTLCompilerService, simulatorDummyPlugin: AIR comes back), then call these device hooks with the service output:
 *   -newVertexVariantWithCompilerOutput:pipelineStatisticsOutput:   (same for Fragment/Compute/Tile)
 *   -newRenderPipelineWithDescriptor:vertexVariant:fragmentVariant:errorMessage:
 *   -newComputePipelineWithDescriptor:variant:errorMessage:
 * AppleParavirtDevice implements exactly these and SnapMetal's AdrenoPipeline.m follows that template; so do we.
 *
 * What differs: the variants are compiled by air2valhall (MaliShader.m), the code and SPDs are uploaded to
 * GPU-executable memory in one 4 GiB window (MaliExecPool, S07 6.13), and blending that the fixed-function
 * blender cannot do becomes a blend shader (air2vh_compile_blend_shader).
 *
 * Which metallib slice Metal hands the compiler is decided by -targetDeviceArchitecture: cpuType 0x01000017 (AIR) with
 * subtype 0xc; QuartzCore's default.metallib slice 0 is 0x01000017/0xc on iOS (spec 05 section 5.1; macOS uses 0xd).
 */
#import "MaliMTLObjectsPrivate.h"
#import <os/log.h>

/* ====================================================================== *
 * Executable memory
 * ====================================================================== */
@implementation MaliExecPool {
    id<MTLDevice> _device;
    NSMutableArray<id<MTLBuffer>> *_chunks;
    id<MTLBuffer> _cur;
    uint32_t _used;
    NSLock *_lock;
}
- (instancetype)initWithDevice:(id<MTLDevice>)device {
    if ((self = [super init])) {
        _device = device;
        _chunks = [NSMutableArray array];
        _lock = [NSLock new];
    }
    return self;
}
- (BOOL)allocate:(uint32_t)size align:(uint32_t)align out:(mali_gpu_ptr_t *)out holder:(id<MTLBuffer> *)holder {
    if (!size) return NO;
    [_lock lock];
    @try {
        uint32_t at = _cur ? (uint32_t)((_used + align - 1) & ~(align - 1)) : 0;
        if (!_cur || (uint64_t)at + size > _cur.length) {
            const uint32_t want = (uint32_t)(((uint64_t)MAX((uint32_t)(1u << 20), size + align) + MALI_PAGE_SIZE - 1) & ~(uint64_t)(MALI_PAGE_SIZE - 1));
            id<MTLBuffer> b = nil;
            for (int attempt = 0; attempt < 4 && !b; attempt++) {
                id<MTLBuffer> t = MaliNewGPUBuffer(_device, want, MALI_BO_GPU_READ | MALI_BO_GPU_WRITE | MALI_BO_GPU_EXEC | MALI_BO_CPU_MAP | MALI_BO_NONCACHEABLE);
                if (!t || !t.contents) return NO;
                if (mali_code_window_ok([(IOGPUMetalBuffer *)t gpuAddress], (uint32_t)MIN((NSUInteger)UINT32_MAX, t.length))) b = t;
                else [_chunks addObject:t];            /* straddles a 4 GiB boundary: keep it unused, try a fresh one */
            }
            if (!b) return NO;
            [_chunks addObject:b];
            _cur = b;
            at = 0;
        }
        out->cpu = (uint8_t *)_cur.contents + at;
        out->gpu = [(IOGPUMetalBuffer *)_cur gpuAddress] + at;
        _used = at + size;
        *holder = _cur;
        return YES;
    } @finally { [_lock unlock]; }
}
@end

/* Copy a finished program into executable memory and build its SPDs. */
BOOL MaliUploadShader(MaliMTLDevice *dev, MaliShaderProgram *p, mali_shader_t *sh, NSMutableArray *holders, NSString **err) {
    memset(sh, 0, sizeof *sh);
    memcpy(&sh->prog, &p->prog, sizeof sh->prog);
    mali_gpu_ptr_t code, spd;
    id<MTLBuffer> h1 = nil, h2 = nil;
    const uint32_t size = (uint32_t)p.code.length;
    if (![[dev maliExecPool] allocate:size align:AIR2VH_CODE_ALIGN out:&code holder:&h1] ||
        ![[dev maliExecPool] allocate:MALI_SPD_MEM_BYTES align:32 out:&spd holder:&h2]) {
        if (err) *err = @"out of GPU-executable memory";
        return NO;
    }
    memcpy(code.cpu, p.code.bytes, size);
    sh->code_gpu = code.gpu;
    if (!mali_code_window_ok(code.gpu, size)) { if (err) *err = @"shader code straddles a 4 GiB window"; return NO; }
    if (!mali_shader_build_spds(sh, spd)) { if (err) *err = @"shader program descriptors could not be built"; return NO; }
    sh->vary_size_B = (p->prog.stage == AIR2VH_STAGE_VERTEX) ? mali_vs_vary_size(&p->layout) : 0;
    sh->valid = true;
    mali_cpu_barrier();
    if (h1 && ![holders containsObject:h1]) [holders addObject:h1];
    if (h2 && ![holders containsObject:h2]) [holders addObject:h2];
    return YES;
}

/* ====================================================================== *
 * Variants (what the compiler service returned for one stage)
 * ====================================================================== */
@implementation MaliFunctionVariant
- (instancetype)initWithCompilerOutput:(id)o stage:(NSUInteger)s {
    if ((self = [super initWithCompilerOutput:o])) {
        _compilerOutput = o;
        _stage = s;
        NSString *error = nil;
        _input = [MaliCompilerInput inputWithCompilerOutput:o stage:s entry:nil error:&error];
        _inputError = error;
    }
    return self;
}
@end

/* ====================================================================== *
 * Pipeline states
 * ====================================================================== */
@implementation MaliRenderPipelineState {
    mali_gfx_pipeline_t _pipe;
    NSMutableArray *_res;
    MaliShaderProgram *_vs, *_fs;
    uint32_t _attrMask;
}
- (instancetype)initWithDevice:(id)d pipelineStateDescriptor:(id)desc {
    if ((self = [super initWithDevice:d pipelineStateDescriptor:desc])) {
        memset(&_pipe, 0, sizeof _pipe);
        _res = [NSMutableArray array];
    }
    return self;
}
- (mali_gfx_pipeline_t *)maliMutablePipe { return &_pipe; }
- (const mali_gfx_pipeline_t *)maliPipe { return &_pipe; }
- (NSMutableArray *)maliMutableResources { return _res; }
- (NSArray *)maliResources { return _res; }
- (void)maliSetVS:(MaliShaderProgram *)v FS:(MaliShaderProgram *)f { _vs = v; _fs = f; _attrMask = v ? v->prog.vs.attribute_mask : 0; }
- (uint32_t)maliVertexAttrMask { return _attrMask; }
- (const MaliConstSampler *)maliConstSamplers:(uint32_t *)count stage:(int)stage {
    MaliShaderProgram *p = stage == 0 ? _vs : _fs;
    *count = p ? p->csamp_count : 0;
    return p ? p->csamp : NULL;
}
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (BOOL)threadgroupSizeMatchesTileSize { return NO; }
- (NSUInteger)imageblockSampleLength { return 0; }
@end

@implementation MaliComputePipelineState {
    mali_compute_pipeline_t _pipe, _nonUniform;
    BOOL _hasNonUniform;
    NSMutableArray *_res, *_resNU;
    MaliShaderProgram *_prog;
    MaliCompilerInput *_input;
    NSUInteger _maxThreads;
}
- (instancetype)initWithDevice:(id)d pipelineStateDescriptor:(id)desc {
    if ((self = [super initWithDevice:d pipelineStateDescriptor:desc])) {
        memset(&_pipe, 0, sizeof _pipe);
        _res = [NSMutableArray array];
        _resNU = [NSMutableArray array];
        _maxThreads = 1024;
    }
    return self;
}
- (mali_compute_pipeline_t *)maliMutablePipe { return &_pipe; }
- (const mali_compute_pipeline_t *)maliPipe { return &_pipe; }
- (NSMutableArray *)maliMutableResources { return _res; }
- (NSArray *)maliResources { return _res; }
- (NSArray *)maliNonUniformResources { return _resNU; }
- (void)maliSetProgram:(MaliShaderProgram *)p input:(MaliCompilerInput *)in maxThreads:(NSUInteger)m { _prog = p; _input = in; _maxThreads = m; _pipe.tg_threads_hint = (uint32_t)m; }
- (const MaliConstSampler *)maliConstSamplers:(uint32_t *)count { *count = _prog ? _prog->csamp_count : 0; return _prog ? _prog->csamp : NULL; }
- (const mali_compute_pipeline_t *)maliNonUniformPipe:(NSString **)error {
    @synchronized(self) {
        if (_hasNonUniform) return &_nonUniform;
        MaliMTLDevice *dev = (MaliMTLDevice *)self.device;
        MaliShaderProgram *p = [[dev maliShaderCompiler] compile:_input flags:AIR2VH_F_NONUNIFORM_DISPATCH vsLayout:nil error:error];
        if (!p) return NULL;
        memset(&_nonUniform, 0, sizeof _nonUniform);
        if (!MaliUploadShader(dev, p, &_nonUniform.cs, _resNU, error)) return NULL;
        _nonUniform.tg_threads_hint = _pipe.tg_threads_hint;
        _hasNonUniform = YES;
        return &_nonUniform;
    }
}
- (NSUInteger)maxTotalThreadsPerThreadgroup { return _maxThreads; }
- (NSUInteger)threadExecutionWidth { return 16; }          /* Valhall warp width (G715) */
- (NSUInteger)staticThreadgroupMemoryLength { return 0; }
@end

/* ====================================================================== *
 * Device hooks
 * ====================================================================== */
static BOOL uses_rgb_blend_constant(const mali_rt_state_t *rt) {
    /* MTLBlendFactorBlendColor 11, OneMinusBlendColor 12: per-channel constants a fixed-function equation cannot always hold */
    const uint32_t f[4] = {rt->src_rgb, rt->dst_rgb, rt->src_alpha, rt->dst_alpha};
    for (int i = 0; i < 4; i++) if (f[i] == 11 || f[i] == 12) return YES;
    return NO;
}

/* Tier-A FS (LD_VAR): T0[i] describes FS varying i, linked by Metal name to the VS output it reads. */
static BOOL link_varyings(MaliShaderProgram *vsp, MaliShaderProgram *fsp, mali_gfx_pipeline_t *gp, NSString **err) {
    const air2vh_program *vs = vsp.programWithExt.bytes, *fs = fsp.programWithExt.bytes;
    air2vh_varying_link links[AIR2VH_EXT_MAX_VARYINGS];
    uint32_t generic = 0;
    const int n = vs && fs ? air2vh_link_varyings(vs, fs, links, AIR2VH_EXT_MAX_VARYINGS, &generic) : -1;
    if (n < 0) { if (err) *err = @"varying link failed: a program carries no varying tables"; return NO; }
    const air2vh_program_ext *fe = air2vh_program_get_ext(fs);
    uint32_t count = fsp->prog.fs.varying_desc_count;
    for (int i = 0; i < n; i++) {
        const air2vh_varying_link *l = &links[i];
        const char *name = fe->varyings[l->fs_index].name;
        if (!l->matched) {
            if (err) *err = [NSString stringWithFormat:@"fragment input %s is not written by the vertex function", name];
            return NO;
        }
        if (l->fs_index >= AIR2VH_EXT_MAX_VARYINGS ||
            !mali_emit_varying_desc(l->alu_type, l->ncomps, l->vs_offset, generic, gp->fs_vary_desc[l->fs_index])) {
            if (err) *err = [NSString stringWithFormat:@"varying %s: type 0x%x x%u has no varying format", name, l->alu_type, l->ncomps];
            return NO;
        }
        count = MAX(count, l->fs_index + 1);
    }
    if (count > AIR2VH_EXT_MAX_VARYINGS) { if (err) *err = @"fragment function reads more varyings than T0 holds"; return NO; }
    gp->fs_vary_count = count;
    return YES;
}

@implementation MaliMTLDevice (Pipeline)
- (int)llvmVersion { return 32023; }                          /* same value AppleParavirtDevice returns (spec 04) */
- (MTLTargetDeviceArchitecture *)targetDeviceArchitecture {
    MTLTargetDeviceArchitecture *a = [MTLTargetDeviceArchitecture new];
    a.cpuType = 0x01000017;                                   /* AIR: pick the air64 slice of fat metallibs */
    a.subType = 0xc;                                          /* iOS QuartzCore slice 0 (macOS: 0xd) */
    a.driverVersion = 1;
    a.version = (MaliArchVersion){1, 0, 0};
    return a;
}
- (id)newVertexVariantWithCompilerOutput:(id)o pipelineStatisticsOutput:(id)s { return [[MaliFunctionVariant alloc] initWithCompilerOutput:o stage:0]; }
- (id)newFragmentVariantWithCompilerOutput:(id)o pipelineStatisticsOutput:(id)s { return [[MaliFunctionVariant alloc] initWithCompilerOutput:o stage:1]; }
- (id)newComputeVariantWithCompilerOutput:(id)o pipelineStatisticsOutput:(id)s { return [[MaliFunctionVariant alloc] initWithCompilerOutput:o stage:2]; }
- (id)newTileVariantWithCompilerOutput:(id)o pipelineStatisticsOutput:(id)s functionType:(unsigned long long)t { return nil; }
- (id)newRenderPipelineWithTileDescriptor:(id)d tileVariant:(id)v errorMessage:(NSString **)err { return nil; }

- (id)newRenderPipelineWithDescriptor:(MTLRenderPipelineDescriptor *)d vertexVariant:(MaliFunctionVariant *)v
                      fragmentVariant:(MaliFunctionVariant *)f errorMessage:(NSString **)err {
    NSString *why = nil;
    id p = [self maliNewRenderPipelineWithDescriptor:d vertexVariant:v fragmentVariant:f errorMessage:&why];
    if (!p)
        MaliDiagLine([NSString stringWithFormat:@"render pipeline %@/%@ refused: %@", d.vertexFunction.name, d.fragmentFunction.name, why]);
    if (err) *err = why;
    return p;
}

- (id)maliNewRenderPipelineWithDescriptor:(MTLRenderPipelineDescriptor *)d vertexVariant:(MaliFunctionVariant *)v
                          fragmentVariant:(MaliFunctionVariant *)f errorMessage:(NSString **)err {
    MaliShaderCompiler *comp = [self maliShaderCompiler];
    if (!comp) { if (err) *err = @"the air2valhall compiler is not available"; return nil; }
    /* Metal lowers the descriptor's vertex fetch into a generated entry (air.vertexFetchFunction) whose AIR calls the
     * requested shader after loading the declared attributes; that is what the service returns, so accept it. */
    const BOOL vertexEntryMatches = [v.input.entry isEqualToString:d.vertexFunction.name] ||
        (d.vertexDescriptor && v.input.stage == 0 && [v.input.entry isEqualToString:@"air.vertexFetchFunction"]);
    const BOOL hasFragment = d.fragmentFunction != nil && d.rasterizationEnabled;
    if (!v.input || (hasFragment && !f.input) || !vertexEntryMatches || (hasFragment && ![f.input.entry isEqualToString:d.fragmentFunction.name])) {
        if (err) *err = v.inputError ?: f.inputError ?: @"compiler variant does not match the render pipeline entry";
        return nil;
    }
    if (d.rasterSampleCount > 1) { if (err) *err = @"MSAA pipelines are not supported (v1)"; return nil; }

    MaliShaderProgram *vsp = [comp compile:v.input flags:0 vsLayout:nil error:err];
    if (!vsp) return nil;
    if (vsp->prog.vs.attribute_mask) { if (err) *err = @"vertex function reads stage_in attributes (T0 attribute tables are not driven in v1)"; return nil; }
    MaliShaderProgram *fsp = nil;
    if (hasFragment) {
        fsp = [comp compile:f.input flags:0 vsLayout:vsp error:err];
        if (!fsp) return nil;
    }

    MaliRenderPipelineState *ps = [[MaliRenderPipelineState alloc] initWithDevice:self pipelineStateDescriptor:d];
    mali_gfx_pipeline_t *gp = [ps maliMutablePipe];
    NSMutableArray *res = [ps maliMutableResources];
    if (!MaliUploadShader(self, vsp, &gp->vs, res, err)) return nil;
    if (fsp && !MaliUploadShader(self, fsp, &gp->fs, res, err)) return nil;
    if (fsp && fsp->prog.fs.uses_ld_var && !link_varyings(vsp, fsp, gp, err)) return nil;

    gp->sample_count = 1;
    gp->alpha_to_coverage = d.alphaToCoverageEnabled;
    gp->alpha_to_one = d.alphaToOneEnabled;
    gp->rasterization_enabled = d.rasterizationEnabled;
    gp->depth_format = (uint32_t)d.depthAttachmentPixelFormat;
    gp->stencil_format = (uint32_t)d.stencilAttachmentPixelFormat;
    gp->input_prim_class = (uint32_t)d.inputPrimitiveTopology;

    const float zero[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < MALI_MAX_RTS; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[i];
        mali_rt_state_t *rt = &gp->rt[i];
        rt->mtl_format = (uint32_t)ca.pixelFormat;
        if (!rt->mtl_format) continue;
        rt->blend_enable = ca.blendingEnabled;
        rt->src_rgb = (uint32_t)ca.sourceRGBBlendFactor;
        rt->dst_rgb = (uint32_t)ca.destinationRGBBlendFactor;
        rt->op_rgb = (uint32_t)ca.rgbBlendOperation;
        rt->src_alpha = (uint32_t)ca.sourceAlphaBlendFactor;
        rt->dst_alpha = (uint32_t)ca.destinationAlphaBlendFactor;
        rt->op_alpha = (uint32_t)ca.alphaBlendOperation;
        rt->write_mask = (uint32_t)ca.writeMask;
        gp->rt_count = i + 1;
        mali_fmt_info_t fi;
        if (!mali_fmt_lookup(rt->mtl_format, &fi) || !(fi.flags & MALI_FMT_COLOR_RT)) {
            if (err) *err = [NSString stringWithFormat:@"color attachment %u: pixel format %lu cannot be rendered to", i, (unsigned long)ca.pixelFormat];
            return nil;
        }
        if (fsp && fsp->prog.fs.rt[i].written && (mali_blend_rt_needs_shader(rt, zero) || (rt->blend_enable && uses_rgb_blend_constant(rt)))) {
            uint8_t keybuf[256];
            const size_t ksz = mali_blend_shader_key(rt, i, fsp->prog.fs.rt[i].base_type, keybuf, sizeof keybuf);
            if (!ksz) { if (err) *err = @"blend shader key could not be built"; return nil; }
            MaliShaderProgram *bp = [comp compileBlendShaderKey:[NSData dataWithBytes:keybuf length:ksz] error:err];
            if (!bp) return nil;
            mali_gpu_ptr_t code;
            id<MTLBuffer> holder = nil;
            if (![[self maliExecPool] allocate:(uint32_t)bp.code.length align:AIR2VH_CODE_ALIGN out:&code holder:&holder]) {
                if (err) *err = @"out of GPU-executable memory (blend shader)";
                return nil;
            }
            memcpy(code.cpu, bp.code.bytes, bp.code.length);
            gp->blend_shader_gpu[i] = code.gpu;
            if (holder && ![res containsObject:holder]) [res addObject:holder];
        }
    }

    MTLVertexDescriptor *vd = d.vertexDescriptor;
    if (vd) {
        for (uint32_t i = 0; i < MALI_MAX_VATTRS; i++) {
            MTLVertexAttributeDescriptor *a = vd.attributes[i];
            if (a.format == MTLVertexFormatInvalid) continue;
            gp->vattr_mask |= 1u << i;
            gp->vattr[i] = (mali_vertex_attr_t){(uint32_t)a.format, (uint32_t)a.offset, (uint32_t)a.bufferIndex};
        }
        for (uint32_t i = 0; i < MALI_MAX_BUFFERS; i++) {
            MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
            gp->vlayout[i] = (mali_vertex_layout_t){(uint32_t)l.stride, (uint32_t)l.stepFunction, (uint32_t)l.stepRate};
        }
    }
    [ps maliSetVS:vsp FS:fsp];
    mali_cpu_barrier();
    return ps;
}

- (id)newComputePipelineWithDescriptor:(MTLComputePipelineDescriptor *)d variant:(MaliFunctionVariant *)v errorMessage:(NSString **)err {
    NSString *why = nil;
    id p = [self maliNewComputePipelineWithDescriptor:d variant:v errorMessage:&why];
    if (!p)
        MaliDiagLine([NSString stringWithFormat:@"compute pipeline %@ refused: %@", d.computeFunction.name, why]);
    if (err) *err = why;
    return p;
}

- (id)maliNewComputePipelineWithDescriptor:(MTLComputePipelineDescriptor *)d variant:(MaliFunctionVariant *)v
                              errorMessage:(NSString **)err {
    MaliShaderCompiler *comp = [self maliShaderCompiler];
    if (!comp) { if (err) *err = @"the air2valhall compiler is not available"; return nil; }
    if (!v.input || ![v.input.entry isEqualToString:d.computeFunction.name]) {
        if (err) *err = v.inputError ?: @"compiler variant does not match the compute pipeline entry";
        return nil;
    }
    MaliShaderProgram *p = [comp compile:v.input flags:0 vsLayout:nil error:err];
    if (!p) return nil;
    MaliComputePipelineState *ps = [[MaliComputePipelineState alloc] initWithDevice:self pipelineStateDescriptor:d];
    if (!MaliUploadShader(self, p, &[ps maliMutablePipe]->cs, [ps maliMutableResources], err)) return nil;
    /* threads per threadgroup the program allows: registers per thread limit the resident threads per core */
    const mali_dev_props_t *dp = &[self maliCtx]->props;
    NSUInteger cap = dp->max_threads_per_core ? dp->max_threads_per_core : 2048;
    if (p->prog.work_reg_count[0] > 32) cap /= 2;
    const NSUInteger wg = dp->max_threads_per_wg ? dp->max_threads_per_wg : 1024;
    [ps maliSetProgram:p input:v.input maxThreads:MIN(MIN(cap, wg), (NSUInteger)1024)];
    mali_cpu_barrier();
    return ps;
}
@end
