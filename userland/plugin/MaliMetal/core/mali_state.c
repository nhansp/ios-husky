/*
 * mali_state.c - Metal fixed-function state -> Valhall draw descriptors/flags
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/core/mali_state.c
 *
 * Translates what a Metal render command encoder holds (blend, depth/stencil,
 * cull, winding, primitive type, scissor, viewport) into the values the IDVS
 * staging registers and the DEPTH_STENCIL / BLEND descriptors carry. The
 * policy for every flag is panvk's (cited), restricted to single-sampled
 * rendering: Metal's CA-scope draws never need MSAA, OQ, conservative
 * rasterisation or layered rendering (spec 05 section 7).
 */
#include "mali_mesa.h"
#include "mali_core.h"
#include "mali_internal.h"

#include <string.h>

#include "util/blend.h"
#include "pan_shader.h"

/* ---------------------------------------------------------------------- *
 * Metal enum -> pipe/Mali enum helpers
 * ---------------------------------------------------------------------- */
static enum pipe_blendfactor
blend_factor(uint32_t f)   /* MTLBlendFactor */
{
    switch (f) {
    case 0:  return PIPE_BLENDFACTOR_ZERO;
    case 1:  return PIPE_BLENDFACTOR_ONE;
    case 2:  return PIPE_BLENDFACTOR_SRC_COLOR;
    case 3:  return PIPE_BLENDFACTOR_INV_SRC_COLOR;
    case 4:  return PIPE_BLENDFACTOR_SRC_ALPHA;
    case 5:  return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
    case 6:  return PIPE_BLENDFACTOR_DST_COLOR;
    case 7:  return PIPE_BLENDFACTOR_INV_DST_COLOR;
    case 8:  return PIPE_BLENDFACTOR_DST_ALPHA;
    case 9:  return PIPE_BLENDFACTOR_INV_DST_ALPHA;
    case 10: return PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case 11: return PIPE_BLENDFACTOR_CONST_COLOR;
    case 12: return PIPE_BLENDFACTOR_INV_CONST_COLOR;
    case 13: return PIPE_BLENDFACTOR_CONST_ALPHA;
    case 14: return PIPE_BLENDFACTOR_INV_CONST_ALPHA;
    case 15: return PIPE_BLENDFACTOR_SRC1_COLOR;
    case 16: return PIPE_BLENDFACTOR_INV_SRC1_COLOR;
    case 17: return PIPE_BLENDFACTOR_SRC1_ALPHA;
    case 18: return PIPE_BLENDFACTOR_INV_SRC1_ALPHA;
    default: return PIPE_BLENDFACTOR_ONE;
    }
}

static enum pipe_blend_func
blend_op(uint32_t op)      /* MTLBlendOperation */
{
    switch (op) {
    case 0: return PIPE_BLEND_ADD;
    case 1: return PIPE_BLEND_SUBTRACT;
    case 2: return PIPE_BLEND_REVERSE_SUBTRACT;
    case 3: return PIPE_BLEND_MIN;
    default: return PIPE_BLEND_MAX;
    }
}

/* MTLColorWriteMask: Red 8, Green 4, Blue 2, Alpha 1 -> PIPE_MASK_R 1, G 2, B 4, A 8 */
static unsigned
color_mask(uint32_t m)
{
    return ((m & 8) ? 1 : 0) | ((m & 4) ? 2 : 0) | ((m & 2) ? 4 : 0) | ((m & 1) ? 8 : 0);
}

static enum mali_stencil_op
stencil_op(uint32_t op)    /* MTLStencilOperation */
{
    switch (op) {
    case 0: return MALI_STENCIL_OP_KEEP;
    case 1: return MALI_STENCIL_OP_ZERO;
    case 2: return MALI_STENCIL_OP_REPLACE;
    case 3: return MALI_STENCIL_OP_INCR_SAT;
    case 4: return MALI_STENCIL_OP_DECR_SAT;
    case 5: return MALI_STENCIL_OP_INVERT;
    case 6: return MALI_STENCIL_OP_INCR_WRAP;
    default: return MALI_STENCIL_OP_DECR_WRAP;
    }
}

static enum mali_draw_mode
draw_mode(uint32_t prim)   /* MTLPrimitiveType */
{
    switch (prim) {
    case 0: return MALI_DRAW_MODE_POINTS;
    case 1: return MALI_DRAW_MODE_LINES;
    case 2: return MALI_DRAW_MODE_LINE_STRIP;
    case 4: return MALI_DRAW_MODE_TRIANGLE_STRIP;
    default: return MALI_DRAW_MODE_TRIANGLES;
    }
}

static inline bool prim_is_triangle(uint32_t prim) { return prim == 3 || prim == 4; }
static inline bool prim_is_strip(uint32_t prim) { return prim == 2 || prim == 4; }

/* ---------------------------------------------------------------------- *
 * Blend descriptors  [M:vulkan/panvk_vX_blend.c:96-260 (v >= 9 branches)]
 * ---------------------------------------------------------------------- */
static void
fill_equation(const mali_rt_state_t *rt, bool is_float, struct pan_blend_equation *eq)
{
    memset(eq, 0, sizeof(*eq));
    eq->blend_enable = rt->blend_enable;
    eq->is_float = is_float;
    eq->color_mask = color_mask(rt->write_mask);
    eq->rgb_func = blend_op(rt->op_rgb);
    eq->rgb_src_factor = blend_factor(rt->src_rgb);
    eq->rgb_dst_factor = blend_factor(rt->dst_rgb);
    eq->alpha_func = blend_op(rt->op_alpha);
    eq->alpha_src_factor = blend_factor(rt->src_alpha);
    eq->alpha_dst_factor = blend_factor(rt->dst_alpha);
}

bool
mali_blend_rt_needs_shader(const mali_rt_state_t *rt, const float constants[4])
{
    mali_fmt_info_t fi;
    if (!rt->mtl_format || !mali_fmt_lookup(rt->mtl_format, &fi))
        return false;
    const enum pipe_format fmt = (enum pipe_format)fi.pipe_format;
    struct pan_blend_equation eq;
    fill_equation(rt, util_format_is_float(fmt), &eq);
    if (!eq.color_mask)
        return false;
    pan_blend_optimize_equation(&eq, fmt, constants);
    if (pan_blend_is_opaque(eq))
        return false;                              /* opaque never needs a shader */
    if (!GENX(pan_format_supports_hw_blend)(fmt))
        return true;
    if (!pan_blend_can_fixed_function(MALI_ARCH, eq, pan_blend_supports_2src(MALI_ARCH)))
        return true;
    const unsigned mask = pan_blend_constant_mask(eq);
    return mask && !pan_blend_is_homogenous_constant(mask, constants);
}

/* The key the compiler library's air2vh_compile_blend_shader() takes: the raw bytes of
 * Mesa's struct pan_blend_shader_key, exactly what panvk hashes [M:vulkan/panvk_vX_blend.c:36-47]. */
size_t
mali_blend_shader_key(const mali_rt_state_t *rt, unsigned rt_index, uint8_t src0_nir_type,
                      void *out_key, size_t cap)
{
    mali_fmt_info_t fi;
    if (!rt->mtl_format || !mali_fmt_lookup(rt->mtl_format, &fi) || cap < sizeof(struct pan_blend_shader_key))
        return 0;
    struct pan_blend_shader_key key;
    memset(&key, 0, sizeof(key));
    key.format = (enum pipe_format)fi.pipe_format;
    key.src0_type = (nir_alu_type)src0_nir_type;
    key.src1_type = nir_type_invalid;
    key.rt = rt_index;
    key.nr_samples = 1;
    fill_equation(rt, util_format_is_float(key.format), &key.equation);
    const float zero[4] = {0, 0, 0, 0};
    pan_blend_optimize_equation(&key.equation, key.format, zero);
    memcpy(out_key, &key, sizeof(key));
    return sizeof(key);
}

/* One BLEND descriptor (16 bytes, 4 words) for RT `i`. */
static void
emit_blend_desc(const mali_gfx_pipeline_t *pipe, unsigned i, const float constants[4],
                uint16_t ff_constant, uint32_t out[4], bool *dest_read)
{
    const mali_rt_state_t *rt = &pipe->rt[i];
    struct mali_blend_packed *bd = (struct mali_blend_packed *)out;
    mali_fmt_info_t fi;
    const bool unused = !rt->mtl_format || !mali_fmt_lookup(rt->mtl_format, &fi) ||
                        !pipe->fs.valid || !pipe->fs.prog.fs.rt[i].written;
    struct pan_blend_equation eq;
    enum pipe_format fmt = PIPE_FORMAT_NONE;
    if (!unused) {
        fmt = (enum pipe_format)fi.pipe_format;
        fill_equation(rt, util_format_is_float(fmt), &eq);
        pan_blend_optimize_equation(&eq, fmt, constants);
    }
    pan_pack(bd, BLEND, cfg) {
        if (unused || !eq.color_mask) {
            cfg.enable = false;
            cfg.internal.mode = MALI_BLEND_MODE_OFF;
            continue;
        }
        cfg.srgb = util_format_is_srgb(fmt);
        cfg.load_destination = pan_blend_reads_dest(eq);
        cfg.round_to_fb_precision = true;
        cfg.blend_constant = ff_constant;
        if (pipe->blend_shader_gpu[i]) {
            const uint64_t bs = pipe->blend_shader_gpu[i];
            cfg.internal.mode = MALI_BLEND_MODE_SHADER;
            cfg.internal.shader.pc = (uint32_t)bs;
            /* blend + fragment code share one 4 GiB region; blend shader 16-byte aligned [panvk_vX_blend.c:96-100] */
        } else {
            const bool opaque = pan_blend_is_opaque(eq);
            cfg.internal.mode = opaque ? MALI_BLEND_MODE_OPAQUE : MALI_BLEND_MODE_FIXED_FUNCTION;
            pan_blend_to_fixed_function_equation(eq, &cfg.equation);
            cfg.internal.fixed_function.num_comps = 4;
            cfg.internal.fixed_function.conversion.memory_format =
                GENX(pan_dithered_format_from_pipe_format)(fmt, false);
            /* fixed function does not like RGB1 as the component order [panvk_vX_blend.c:158-166] */
            if (cfg.internal.mode == MALI_BLEND_MODE_FIXED_FUNCTION &&
                (cfg.internal.fixed_function.conversion.memory_format & 0xff) == MALI_RGB_COMPONENT_ORDER_RGB1) {
                cfg.internal.fixed_function.conversion.memory_format &= ~0xffu;
                cfg.internal.fixed_function.conversion.memory_format |= MALI_RGB_COMPONENT_ORDER_RGBA;
            }
            cfg.internal.fixed_function.rt = i;
        }
    }
    if (!unused && eq.color_mask && dest_read && pan_blend_reads_dest(eq))
        *dest_read = true;
}

/* FAU sysval rt_conv[]: the LD_TILE conversion word of each RT a framebuffer-fetch shader reads,
 * sized like the colour output [M:gallium/drivers/panfrost/pan_cmdstream.c panfrost_upload_rt_conversion_sysval] */
void
mali_state_rt_conversions(const mali_gfx_pipeline_t *pipe, uint32_t out[MALI_MAX_RTS])
{
    for (unsigned i = 0; i < MALI_MAX_RTS; i++) {
        mali_fmt_info_t fi;
        if (i < pipe->rt_count && pipe->rt[i].mtl_format && mali_fmt_lookup(pipe->rt[i].mtl_format, &fi)) {
            const unsigned size = (pipe->fs.prog.fs.rt[i].base_type & 0x79u) == 16u ? 16u : 32u;
            out[i] = (uint32_t)(GENX(pan_blend_get_internal_desc)((enum pipe_format)fi.pipe_format, i, size, false) >> 32);
        } else {
            pan_cast_and_pack(&out[i], INTERNAL_CONVERSION, cfg)
                cfg.memory_format = GENX(pan_format_from_pipe_format)(PIPE_FORMAT_NONE)->hw;
        }
    }
}

bool
mali_state_emit_blend(mali_arena_t *a, const mali_gfx_pipeline_t *pipe, const float constants[4],
                      uint64_t *bds_reg, uint64_t blend_descs_out[MALI_MAX_RTS], bool *any_dest_read)
{
    const unsigned n = pipe->rt_count ? pipe->rt_count : 1;     /* the hardware wants >= 1 RT [S06: panvk rt_count = MAX2(count,1)] */
    mali_gpu_ptr_t m = mali_arena_alloc(a, n * 16, 64);
    if (!m.cpu)
        return false;
    /* FF blend constant (v7+ reads RT0's): the first RT that uses one, as 16-bit unorm/float [panvk_vX_blend.c:199-244] */
    uint16_t ff_const = 0;
    for (unsigned i = 0; i < n; i++) {
        const mali_rt_state_t *rt = &pipe->rt[i];
        mali_fmt_info_t fi;
        if (!rt->mtl_format || !rt->blend_enable || !mali_fmt_lookup(rt->mtl_format, &fi))
            continue;
        struct pan_blend_equation eq;
        fill_equation(rt, util_format_is_float((enum pipe_format)fi.pipe_format), &eq);
        pan_blend_optimize_equation(&eq, (enum pipe_format)fi.pipe_format, constants);
        const unsigned mask = pan_blend_constant_mask(eq);
        if (mask) {
            ff_const = pan_pack_blend_constant((enum pipe_format)fi.pipe_format,
                                               pan_blend_get_constant(mask, constants));
            break;
        }
    }
    uint32_t (*bd)[4] = m.cpu;
    if (any_dest_read)
        *any_dest_read = false;
    for (unsigned i = 0; i < n; i++)
        emit_blend_desc(pipe, i, constants, ff_const, bd[i], any_dest_read);
    /* FAU sysval blend_descs[loc] = words 2..3 of the packed descriptor [panvk_vX_blend.c:392-410] */
    for (unsigned i = 0; i < MALI_MAX_RTS; i++) {
        uint32_t w[4];
        if (i < n) {
            memcpy(w, bd[i], 16);
        } else {
            struct mali_blend_packed off;
            pan_pack(&off, BLEND, cfg) { cfg.enable = false; cfg.internal.mode = MALI_BLEND_MODE_OFF; }
            memcpy(w, &off, 16);
        }
        blend_descs_out[i] = (uint64_t)w[2] | ((uint64_t)w[3] << 32);
    }
    *bds_reg = m.gpu | n;       /* BLEND_DESC = array | count [panvk_vX_cmd_draw.c:864] */
    return true;
}

/* ---------------------------------------------------------------------- *
 * DCD flags and primitive flags
 * ---------------------------------------------------------------------- */
static bool
stencil_face_writes(const mali_stencil_face_t *f)
{
    return f->write_mask && (f->fail_op != 0 || f->depth_fail_op != 0 || f->pass_op != 0);
}

void
mali_state_dcd(const mali_gfx_pipeline_t *pipe, const mali_draw_state_t *s, bool any_dest_read,
               uint32_t out[3], struct pan_earlyzs_state *ez_out)
{
    const bool has_fs = pipe->fs.valid;
    const air2vh_program *fp = &pipe->fs.prog;
    const bool triangles = prim_is_triangle(s->prim);

    uint32_t rt_written = 0, rt_mask = 0;
    for (unsigned i = 0; i < pipe->rt_count; i++) {
        if (pipe->rt[i].mtl_format)
            rt_mask |= 1u << i;
        if (has_fs && fp->fs.rt[i].written)
            rt_written |= 1u << i;
    }
    const uint32_t rt_read = has_fs ? (fp->fs.fb_fetch_rt_mask & 0xff) : 0;

    const mali_zs_state_t *zs = s->zs;
    const bool has_depth = pipe->depth_format != 0;
    const bool has_stencil = pipe->stencil_format != 0;
    const bool test_z = zs && has_depth;
    const bool writes_z = test_z && zs->depth_write;
    const bool test_s = zs && has_stencil && zs->stencil_enable;
    const bool writes_s = test_s && (stencil_face_writes(&zs->front) || stencil_face_writes(&zs->back));
    const bool zs_always_passes = !(test_z && zs->depth_compare != 7 /* Always */) && !test_s;
    const bool alpha_to_cov = pipe->alpha_to_coverage;
    const bool shader_modifies_coverage = has_fs && (fp->fs.writes_coverage || fp->fs.can_discard || alpha_to_cov);

    struct pan_earlyzs_state ez = {0};
    struct mali_dcd_flags_0_packed f0;
    pan_pack(&f0, DCD_FLAGS_0, cfg) {
        if (has_fs) {
            /* air2vh_program.fs.earlyzs_lut is states[2][2][2][3], ONE BYTE per state (contract
             * air2vh_api.h; Mesa's own struct pan_earlyzs_state is 4 bytes of bitfields, so the
             * compiler library packs it): byte = update[1:0] | kill[3:2] | shader_readonly_zs[4]. */
            const bool oq = false;      /* visibility results are not supported in v1 */
            const unsigned idx = ((((writes_z || writes_s || oq) ? 1u : 0u) * 2u + (alpha_to_cov ? 1u : 0u)) * 2u +
                                  (zs_always_passes ? 1u : 0u)) * 3u + (unsigned)PAN_EARLYZS_ZS_TILEBUF_NOT_READ;
            const uint8_t eb = fp->fs.earlyzs_lut[idx];
            ez.update = eb & 3u;
            ez.kill = (eb >> 2) & 3u;
            ez.shader_readonly_zs = (eb >> 4) & 1u;
            cfg.allow_forward_pixel_to_kill = fp->fs.can_fpk && !(rt_mask & ~rt_written) &&
                                              !(rt_read & rt_written) && !alpha_to_cov && !any_dest_read;
            cfg.allow_forward_pixel_to_be_killed = !fp->writes_global;
            cfg.pixel_kill_operation = (enum mali_pixel_kill)ez.kill;
            cfg.zs_update_operation = (enum mali_pixel_kill)ez.update;
            /* a blend shader stores one sample per invocation [MV:panvk_vX_cmd_draw.c:2532-2540] */
            bool blend_shader = false;
            for (unsigned i = 0; i < pipe->rt_count; i++)
                blend_shader |= pipe->blend_shader_gpu[i] != 0;
            cfg.evaluate_per_sample = pipe->sample_count > 1 && (fp->fs.sample_shading || blend_shader);
            cfg.shader_modifies_coverage = shader_modifies_coverage;
        } else {
            cfg.allow_forward_pixel_to_kill = true;
            cfg.allow_forward_pixel_to_be_killed = true;
            cfg.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;
            cfg.zs_update_operation = MALI_PIXEL_KILL_FORCE_EARLY;
            cfg.overdraw_alpha0 = true;
            cfg.overdraw_alpha1 = true;
        }
        /* Metal's winding is judged in clip space (y up); our viewport transform
         * flips y, so the hardware sees the opposite winding: invert [S06/S07 probe item] */
        cfg.front_face_ccw = s->winding == 0;
        cfg.cull_front_face = triangles && s->cull_mode == 1;
        cfg.cull_back_face = triangles && s->cull_mode == 2;
        cfg.multisample_enable = pipe->sample_count > 1;
        cfg.occlusion_query = MALI_OCCLUSION_MODE_DISABLED;
        cfg.alpha_to_coverage = alpha_to_cov;
        cfg.scissor_to_bounding_box = true;
        cfg.conservative_rast_mode = MALI_CONSERVATIVE_RAST_MODE_DISABLED;
        cfg.cull_zero_area = true;      /* v11 < v14 [M:vulkan/csf/panvk_vX_cmd_draw.c:2574-2578] */
    }
    struct mali_dcd_flags_1_packed f1;
    pan_pack(&f1, DCD_FLAGS_1, cfg) {
        cfg.sample_mask = 0xffff;
        cfg.render_target_mask = rt_written;
    }
    struct mali_dcd_flags_2_packed f2;
    pan_pack(&f2, DCD_FLAGS_2, cfg) {
        cfg.read_mask = rt_read;
        cfg.write_mask = rt_written;
        if (has_fs) {
            cfg.no_shader_depth_read = true;     /* no input-attachment depth/stencil reads in Metal */
            cfg.no_shader_stencil_read = true;
        }
    }
    out[0] = f0.opaque[0];
    out[1] = f1.opaque[0];
    out[2] = f2.opaque[0];
    if (ez_out)
        *ez_out = ez;
}

void
mali_state_prim_flags(const mali_gfx_pipeline_t *pipe, const mali_draw_state_t *s, uint32_t *flags,
                      uint32_t *flags2, uint32_t *override)
{
    const air2vh_program *vp = &pipe->vs.prog;
    const bool has_fs = pipe->fs.valid;
    const bool points = s->prim == 0;
    /* triangles use the no_psiz variant; points keep point-size writes [panvk_vX_cmd_draw.c:2724-2730] */
    const bool writes_point_size = vp->vs.writes_point_size && points;
    const bool extended_fifo = writes_point_size || vp->vs.needs_extended_fifo;
    const bool clip = s->depth_clip_mode == 0;

    struct mali_primitive_flags_packed pf;
    pan_pack(&pf, PRIMITIVE_FLAGS, cfg) {
        cfg.draw_mode = draw_mode(s->prim);
        cfg.primitive_index_enable = has_fs && pipe->fs.prog.fs.reads_primitive_id;
        cfg.primitive_index_override = false;
        cfg.point_size_array_format = writes_point_size ? MALI_POINT_SIZE_ARRAY_FORMAT_FP16
                                                        : MALI_POINT_SIZE_ARRAY_FORMAT_NONE;
        cfg.layer_index_enable = false;
        cfg.position_fifo_format = extended_fifo ? MALI_FIFO_FORMAT_EXTENDED : MALI_FIFO_FORMAT_BASIC;
        cfg.low_depth_cull = cfg.high_depth_cull = clip;
        cfg.secondary_shader = vp->vs.secondary_enable && has_fs;
        cfg.primitive_restart = s->indexed && prim_is_strip(s->prim);   /* Metal restarts on the max index */
        cfg.view_mask = 0;
    }
    struct mali_primitive_flags_2_packed pf2;
    pan_pack(&pf2, PRIMITIVE_FLAGS_2, cfg) { }
    *flags = pf.opaque[0];
    *flags2 = pf2.opaque[0];

    /* flags_override for RUN_IDVS carries only the index type [panvk_vX_cmd_draw.c:2828-2839] */
    struct mali_primitive_flags_packed ov;
    pan_pack_nodefaults(&ov, PRIMITIVE_FLAGS, cfg) {
        cfg.index_type = !s->indexed ? MALI_INDEX_TYPE_NONE
                                     : (s->index_type == 0 ? MALI_INDEX_TYPE_UINT16 : MALI_INDEX_TYPE_UINT32);
    }
    *override = ov.opaque[0];
}

/* ---------------------------------------------------------------------- *
 * Depth/stencil descriptor  [M:vulkan/csf/panvk_vX_cmd_draw.c:2238-2325]
 * ---------------------------------------------------------------------- */
bool
mali_state_emit_zsd(mali_arena_t *a, const mali_gfx_pipeline_t *pipe, const mali_draw_state_t *s, uint64_t *zsd_gpu)
{
    mali_gpu_ptr_t m = mali_arena_alloc(a, 32, 32);
    if (!m.cpu)
        return false;
    const mali_zs_state_t *zs = s->zs;
    const bool has_fs = pipe->fs.valid;
    const bool has_depth = pipe->depth_format != 0;
    const bool has_stencil = pipe->stencil_format != 0;
    const bool test_z = zs && has_depth;
    const bool test_s = zs && has_stencil && zs->stencil_enable;
    const bool clip = s->depth_clip_mode == 0;
    const bool bias = s->depth_bias != 0.0f || s->depth_slope != 0.0f;

    pan_pack((struct mali_depth_stencil_packed *)m.cpu, DEPTH_STENCIL, cfg) {
        cfg.stencil_test_enable = test_s;
        if (test_s) {
            cfg.front_compare_function = (enum mali_func)zs->front.compare;
            cfg.front_stencil_fail = stencil_op(zs->front.fail_op);
            cfg.front_depth_fail = stencil_op(zs->front.depth_fail_op);
            cfg.front_depth_pass = stencil_op(zs->front.pass_op);
            cfg.back_compare_function = (enum mali_func)zs->back.compare;
            cfg.back_stencil_fail = stencil_op(zs->back.fail_op);
            cfg.back_depth_fail = stencil_op(zs->back.depth_fail_op);
            cfg.back_depth_pass = stencil_op(zs->back.pass_op);
        }
        cfg.stencil_from_shader = has_fs ? pipe->fs.prog.fs.writes_stencil : 0;
        cfg.front_write_mask = zs ? (zs->front.write_mask & 0xff) : 0xff;
        cfg.back_write_mask = zs ? (zs->back.write_mask & 0xff) : 0xff;
        cfg.front_value_mask = zs ? (zs->front.read_mask & 0xff) : 0xff;
        cfg.back_value_mask = zs ? (zs->back.read_mask & 0xff) : 0xff;
        cfg.front_reference_value = s->stencil_ref_front & 0xff;
        cfg.back_reference_value = s->stencil_ref_back & 0xff;

        cfg.depth_cull_enable = clip;
        if (!clip)
            cfg.depth_clamp_mode = MALI_DEPTH_CLAMP_MODE_BOUNDS;
        if (has_fs) {
            cfg.separated_dependency_tracking = true;      /* arch == 11 [panvk_vX_cmd_draw.c:2305-2307] */
            cfg.depth_source = pipe->fs.prog.fs.writes_depth ? MALI_DEPTH_SOURCE_SHADER
                                                             : MALI_DEPTH_SOURCE_FIXED_FUNCTION;
        }
        cfg.depth_write_enable = test_z && zs->depth_write;
        cfg.depth_bias_enable = bias;
        cfg.depth_function = test_z ? (enum mali_func)zs->depth_compare : MALI_FUNC_ALWAYS;
        cfg.depth_units = s->depth_bias;
        cfg.depth_factor = s->depth_slope;
        cfg.depth_bias_clamp = s->depth_clamp;
    }
    *zsd_gpu = m.gpu;
    return true;
}

/* ---------------------------------------------------------------------- *
 * Tiler context  [M:vulkan/csf/panvk_vX_cmd_draw.c:1216-1412; CPU-filled into
 * zeroed memory because a Metal command buffer is one-shot, S06 5.4 step 2]
 * ---------------------------------------------------------------------- */
bool
mali_state_emit_tiler_ctx(mali_arena_t *a, const mali_ctx_t *ctx, uint64_t heap_desc_gpu, uint32_t samples, uint32_t width,
                          uint32_t height, uint32_t tile_size_px, uint64_t *gpu)
{
    mali_gpu_ptr_t m = mali_arena_alloc(a, MALI_TILER_CONTEXT_LENGTH, MALI_TILER_CONTEXT_ALIGN);
    if (!m.cpu)
        return false;
    pan_pack((struct mali_tiler_context_packed *)m.cpu, TILER_CONTEXT, cfg) {
        cfg.hierarchy_mask = GENX(pan_select_tiler_hierarchy_mask)(
            width, height, ctx->tiler_max_levels, tile_size_px, ctx->tiler_chunk_size - 64);
        cfg.fb_width = width;
        cfg.fb_height = height;
        cfg.sample_pattern = pan_sample_pattern(samples);
        cfg.first_provoking_vertex = true;           /* Metal flat shading uses the first vertex */
        cfg.layer_count = 1;
        cfg.layer_offset = 0;
        cfg.heap = heap_desc_gpu + MALI_QSTATE_HEAP_DESC_OFF;
        cfg.geometry_buffer = heap_desc_gpu + MALI_QSTATE_GEOM_OFF;
        cfg.geometry_buffer_size = MALI_QSTATE_GEOM_SIZE;
    }
    *gpu = m.gpu;
    return true;
}
