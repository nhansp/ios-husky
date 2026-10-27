/*
 * mali_desc.c - context, arenas, Valhall descriptors, SPD/SRT/FAU construction
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/core/mali_desc.c
 *
 * Every function here produces bytes the GPU reads directly; the layouts come
 * from the generated genxml v11 packers (pan_pack), never from hand-rolled
 * shifts. Where a value is a Metal-to-Mali policy the Mesa or spec source is
 * cited on the line.
 */
#include "mali_mesa.h"      /* first: see the note in mali_mesa.h */
#include "mali_core.h"
#include "mali_internal.h"

#include <stdlib.h>
#include <string.h>

#include "pan_encoder.h"
#include "pan_pool.h"
#include "pan_shader.h"   /* pan_depth_source, pan_register_allocation */
#include "pan_model.h"

/* ===================================================================== *
 * Context
 * ===================================================================== */
void
mali_dev_props_husky_default(mali_dev_props_t *p)
{
    memset(p, 0, sizeof(*p));
    p->gpu_id = 0xb8a21020u;            /* arch 11.8, product 2, r1p2 [S06 section 2, LOG:152] */
    p->gpu_variant = 4;                  /* CORE_FEATURES & 0xff    [S06 section 2]            */
    p->shader_present = 0x01110055u;     /* 7 cores, id range 25    [LOG:161]                  */
    p->tiler_features = 0x809;           /* bin 2^9 B, 8 levels     [LOG:155]                  */
    p->mem_features = 0x301;
    p->mmu_features = 0x2830;
    p->as_present = 0xff;
    /* Not read from the device yet (S06 section 11 item 1). UPPER BOUNDS, because they size
     * thread-local storage: an under-sized TLS block is a GPU fault, an over-sized one is
     * only memory. The kernel publishes the real values in the MaliDevProps registry blob. */
    p->max_threads_per_core = 2048;
    p->max_threads_per_wg = 1024;
    p->max_tasks_per_core = 8;
    p->num_registers_per_core = 65536;
    p->cs_reg_count = 96;
    p->cs_scoreboard_slots = 16;
    p->glb_version = 0x00020008;
    p->num_csg = 8;
    p->num_cs_per_csg = 8;
    p->group_suspend_size = 0x6700;
    p->pgsize_bitmap = 0x1000;
    p->is_io_coherent = false;
    p->core_count = 7;
    p->core_id_range = 25;
}

int
mali_ctx_init(mali_ctx_t *ctx, const mali_dev_props_t *props)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->props = *props;
    ctx->arch = pan_arch(props->gpu_id);
    if (ctx->arch != MALI_ARCH)
        return -1;      /* this core is built for PAN_ARCH 11 only */

    ctx->nr_regs = props->cs_reg_count ? props->cs_reg_count : 96;
    ctx->nr_kernel_regs = 4;     /* unpreserved_cs_reg_count [S06 section 2] */

    /* A pan_kmod_dev_props is the only thing Mesa's query helpers read. */
    struct pan_kmod_dev_props kp;
    memset(&kp, 0, sizeof(kp));
    kp.gpu_id = props->gpu_id;
    kp.gpu_variant = props->gpu_variant;
    kp.gpu_features = props->gpu_features;
    kp.shader_present = props->shader_present;
    kp.tiler_features = props->tiler_features;
    kp.mem_features = props->mem_features;
    kp.mmu_features = props->mmu_features;
    kp.l2_features = props->l2_features;
    for (int i = 0; i < 4; i++)
        kp.texture_features[i] = props->texture_features[i];
    kp.max_threads_per_core = props->max_threads_per_core;
    kp.max_tasks_per_core = props->max_tasks_per_core;
    kp.max_threads_per_wg = props->max_threads_per_wg;
    kp.num_registers_per_core = props->num_registers_per_core;
    kp.max_tls_instance_per_core = props->max_threads_per_core;   /* v10+: no TLS_ALLOC [S06 5.1] */

    ctx->core_id_range = pan_query_core_id_range(&kp);
    const struct pan_tiler_features tf = pan_query_tiler_features(&kp);
    ctx->tiler_max_levels = tf.max_levels;
    ctx->model = pan_get_model(props->gpu_id, props->gpu_variant, pan_query_core_count(&kp),
                               props->gpu_features);
    if (!ctx->model)
        return -2;
    ctx->tile_rt_budget_B = pan_query_optimal_tib_size(ctx->arch, ctx->model);
    ctx->tile_z_budget_B = pan_query_optimal_z_tib_size(ctx->arch, ctx->model);
    /* Heap chunk size the kernel creates for the queue heap; the hierarchy mask is chosen
     * against chunk-64 [S06 5.4 step 1]. Must equal MaliPluginABI.h MALI_HEAP_CHUNK_SIZE. */
    ctx->tiler_chunk_size = 2u << 20;
    return 0;
}

/* ===================================================================== *
 * Arenas
 * ===================================================================== */
void
mali_arena_init(mali_arena_t *a, const mali_arena_ops_t *ops, void *ops_ctx)
{
    memset(a, 0, sizeof(*a));
    a->ops = ops;
    a->ops_ctx = ops_ctx;
}

mali_gpu_ptr_t
mali_arena_alloc(mali_arena_t *a, uint32_t size, uint32_t align)
{
    mali_gpu_ptr_t none = {NULL, 0};
    if (!size)
        size = 1;
    if (align < 8)
        align = 8;
    /* Chunks are page aligned, so aligning the offset aligns the address up to 4096. */
    uint32_t off = (uint32_t)MALI_ALIGN(a->used, align);
    if (!a->cpu || (uint64_t)off + size > a->size) {
        mali_gpu_ptr_t c;
        uint32_t csize = 0;
        uint32_t need = size + (align > 4096 ? align : 0);
        if (!a->ops || !a->ops->new_chunk || !a->ops->new_chunk(a->ops_ctx, need, &c, &csize))
            return none;
        a->cpu = c.cpu;
        a->gpu = c.gpu;
        a->size = csize;
        a->used = 0;
        a->chunk_count++;
        a->total_bytes += csize;
        off = (uint32_t)MALI_ALIGN(a->gpu, align) - (uint32_t)a->gpu;
        if ((uint64_t)off + size > a->size)
            return none;
    }
    mali_gpu_ptr_t r = {a->cpu + off, a->gpu + off};
    a->used = off + size;
    return r;
}

void
mali_cpu_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

/* ===================================================================== *
 * Small descriptors
 * ===================================================================== */
void
mali_emit_null_desc(uint32_t desc[8])
{
    memset(desc, 0, 32);
}

void
mali_emit_buffer_desc(uint64_t gpu, uint64_t size, uint32_t desc[8])
{
    memset(desc, 0, 32);
    pan_pack((struct mali_buffer_packed *)desc, BUFFER, cfg) {
        cfg.address = gpu;
        cfg.size = (uint32_t)size;
        cfg.size_hi = (uint32_t)(size >> 32);
    }
}

void
mali_emit_resource_entry(uint64_t table_gpu, uint64_t size_B, bool contains_desc, uint32_t out[4])
{
    memset(out, 0, 16);
    pan_pack((struct mali_resource_packed *)out, RESOURCE, cfg) {
        cfg.address = table_gpu;
        cfg.size = size_B;
        cfg.contains_descriptors = contains_desc;
    }
}

/* ---- sampler: Metal -> Mali SAMPLER  [S07 6.6; M:vulkan/panvk_vX_sampler.c:67-180] ---- */
static enum mali_wrap_mode
wrap_from_mtl(uint32_t mode, bool *clamp_zero)
{
    switch (mode) {
    case 0: return MALI_WRAP_MODE_CLAMP_TO_EDGE;            /* MTLSamplerAddressModeClampToEdge        */
    case 1: return MALI_WRAP_MODE_MIRRORED_CLAMP_TO_EDGE;   /* MirrorClampToEdge                        */
    case 2: return MALI_WRAP_MODE_REPEAT;                   /* Repeat                                   */
    case 3: return MALI_WRAP_MODE_MIRRORED_REPEAT;          /* MirrorRepeat                             */
    case 4: *clamp_zero = true; return MALI_WRAP_MODE_CLAMP_TO_BORDER; /* ClampToZero (transparent black) */
    case 5: return MALI_WRAP_MODE_CLAMP_TO_BORDER;          /* ClampToBorderColor                       */
    default: return MALI_WRAP_MODE_CLAMP_TO_EDGE;           /* unknown -> edge, as a8xx does [S07 6.6] */
    }
}

void
mali_emit_sampler(const mali_sampler_params_t *p, bool integer_texture, uint32_t desc[8])
{
    memset(desc, 0, 32);
    bool clamp_zero = false;
    const bool aniso = p->max_anisotropy > 1;
    const bool both_nearest = p->min_filter == 0 && p->mag_filter == 0;

    /* Border colour words: float bits for float textures, integer bits otherwise [S07 6.6] */
    uint32_t border[4];
    uint32_t one = integer_texture ? 1u : 0x3f800000u;
    switch (p->border) {
    default:
    case 0: border[0] = border[1] = border[2] = border[3] = 0; break;           /* transparent black */
    case 1: border[0] = border[1] = border[2] = 0; border[3] = one; break;      /* opaque black      */
    case 2: border[0] = border[1] = border[2] = border[3] = one; break;         /* opaque white      */
    }

    pan_pack((struct mali_sampler_packed *)desc, SAMPLER, cfg) {
        cfg.magnify_nearest = p->mag_filter == 0;
        cfg.minify_nearest = p->min_filter == 0;
        switch (p->mip_filter) {
        case 0:  cfg.mipmap_mode = MALI_MIPMAP_MODE_NONE; break;
        case 1:  cfg.mipmap_mode = MALI_MIPMAP_MODE_NEAREST; break;
        default: cfg.mipmap_mode = aniso ? MALI_MIPMAP_MODE_PERFORMANCE_TRILINEAR : MALI_MIPMAP_MODE_TRILINEAR; break;
        }
        cfg.normalized_coordinates = p->normalized_coords;
        cfg.clamp_integer_array_indices = false;
        if (both_nearest)
            cfg.round_to_nearest_even = false;       /* [M:vulkan/panvk_vX_sampler.c:131-137] */

        cfg.wrap_mode_s = wrap_from_mtl(p->addr_s, &clamp_zero);
        cfg.wrap_mode_t = wrap_from_mtl(p->addr_t, &clamp_zero);
        /* Unnormalised coordinates ignore the r mode and force clamp-to-edge, LOD 0 [S07 6.6 pixel_coords] */
        cfg.wrap_mode_r = p->normalized_coords ? wrap_from_mtl(p->addr_r, &clamp_zero)
                                               : MALI_WRAP_MODE_CLAMP_TO_EDGE;
        cfg.seamless_cube_map = true;

        if (p->normalized_coords) {
            float lmax = p->lod_max;
            if (!(lmax <= 31.99f)) lmax = 31.99f;           /* ulod is 13 bits / 256 [S07 6.6] */
            float lmin = p->lod_min < 0.0f ? 0.0f : (p->lod_min > lmax ? lmax : p->lod_min);
            cfg.minimum_lod = lmin;
            cfg.maximum_lod = lmax;
            cfg.lod_bias = p->lod_bias;
        } else {
            cfg.minimum_lod = 0.0f;
            cfg.maximum_lod = 0.0f;
        }

        /* compare: none -> NEVER; else the hardware compares in the opposite order to the API
         * for LESS/GREATER and LEQUAL/GEQUAL [M:lib/pan_encoder.h:157-175; S07 6.6, [U] probe]. */
        if (p->compare >= 8)
            cfg.compare_function = MALI_FUNC_NEVER;
        else
            cfg.compare_function = pan_flip_compare_func((enum mali_func)p->compare);

        if (clamp_zero) {
            cfg.border_color_r = cfg.border_color_g = cfg.border_color_b = cfg.border_color_a = 0;
        } else {
            cfg.border_color_r = border[0];
            cfg.border_color_g = border[1];
            cfg.border_color_b = border[2];
            cfg.border_color_a = border[3];
        }
        if (aniso) {
            cfg.maximum_anisotropy = p->max_anisotropy > 16 ? 16 : p->max_anisotropy;
            cfg.lod_algorithm = MALI_LOD_ALGORITHM_ANISOTROPIC;
        }
        cfg.reduction_mode = MALI_REDUCTION_MODE_AVERAGE;
    }
}

/* ---- textures ---- */
static const struct pan_image_view *
sampling_view(const mali_texture_t *t, struct pan_image_view *scratch)
{
    const struct pan_image_view *v = mali_texture_pan_view(t);
    if (util_format_is_depth_and_stencil(v->format) || util_format_is_depth_or_stencil(v->format)) {
        *scratch = *v;
        scratch->format = util_format_get_depth_only(v->format);
        return scratch;
    }
    return v;
}

bool
mali_emit_sampled_texture(const mali_texture_t *t, mali_arena_t *a, uint32_t desc[8])
{
    struct pan_image_view scratch;
    const struct pan_image_view *iv = sampling_view(t, &scratch);
    const unsigned size = GENX(pan_texture_estimate_payload_size)(iv);
    const unsigned align = GENX(pan_texture_get_payload_alignment)(iv);
    mali_gpu_ptr_t m = mali_arena_alloc(a, size, align);
    if (!m.cpu)
        return false;
    struct pan_ptr payload = {m.cpu, m.gpu};
    memset(desc, 0, 32);
    GENX(pan_sampled_texture_emit)(iv, (struct mali_texture_packed *)desc, &payload);
    return true;
}

bool
mali_emit_storage_texture(const mali_texture_t *t, mali_arena_t *a, uint32_t desc[8])
{
    const struct pan_image_view *iv = mali_texture_pan_view(t);
    const unsigned size = GENX(pan_texture_estimate_payload_size)(iv);
    const unsigned align = GENX(pan_texture_get_payload_alignment)(iv);
    mali_gpu_ptr_t m = mali_arena_alloc(a, size, align);
    if (!m.cpu)
        return false;
    struct pan_ptr payload = {m.cpu, m.gpu};
    memset(desc, 0, 32);
    GENX(pan_storage_texture_emit)(iv, (struct mali_texture_packed *)desc, &payload);
    return true;
}

/* ===================================================================== *
 * Shader program descriptors  [M:vulkan/panvk_vX_shader.c:1235-1345]
 * ===================================================================== */
bool
mali_code_window_ok(uint64_t code_gpu, uint32_t code_size)
{
    return code_size && (code_gpu >> 32) == ((code_gpu + code_size - 1) >> 32);
}

static enum mali_flush_to_zero_mode
ftz_mode(const air2vh_program *p)
{
    if (p->ftz_fp32)
        return p->ftz_fp16 ? MALI_FLUSH_TO_ZERO_MODE_ALWAYS : MALI_FLUSH_TO_ZERO_MODE_DX11;
    return MALI_FLUSH_TO_ZERO_MODE_PRESERVE_SUBNORMALS;
}

bool
mali_shader_build_spds(mali_shader_t *s, mali_gpu_ptr_t mem)
{
    const air2vh_program *p = &s->prog;
    if (!mem.cpu || (mem.gpu & 31) || !s->code_gpu || (s->code_gpu & 127))
        return false;

    unsigned n = p->spd_count ? p->spd_count : 1;
    if (n > 3)
        return false;
    for (unsigned i = 0; i < n; i++) {
        struct mali_shader_program_packed *spd = (struct mali_shader_program_packed *)((uint8_t *)mem.cpu + 32 * i);
        const unsigned regs = p->work_reg_count[i] ? p->work_reg_count[i] : p->work_reg_count[0];
        const uint64_t preload = p->preload[i] ? p->preload[i] : (i ? p->preload[0] : 0);
        pan_pack(spd, SHADER_PROGRAM, cfg) {
            switch (p->stage) {
            case 1: cfg.stage = MALI_SHADER_STAGE_VERTEX; break;
            case 2: cfg.stage = MALI_SHADER_STAGE_FRAGMENT; break;
            default: cfg.stage = MALI_SHADER_STAGE_COMPUTE; break;
            }
            if (p->stage == 2) {
                cfg.fragment_coverage_bitmask_type = MALI_COVERAGE_BITMASK_TYPE_GL;
                cfg.requires_helper_threads = p->requires_helper_threads;
            } else if (p->stage == 1) {
                /* position variants run at half warp limit, the varying variant at full
                 * [M:vulkan/panvk_vX_shader.c:1291-1341] */
                cfg.vertex_warp_limit = (i == 2) ? MALI_WARP_LIMIT_FULL : MALI_WARP_LIMIT_HALF;
            }
            cfg.register_allocation = regs <= 32 ? MALI_SHADER_REGISTER_ALLOCATION_32_PER_THREAD
                                                 : MALI_SHADER_REGISTER_ALLOCATION_64_PER_THREAD;
            cfg.binary = s->code_gpu + p->entry_offset[i];
            cfg.preload.r48_r63 = (uint32_t)(preload >> 48);
            cfg.flush_to_zero_mode = ftz_mode(p);
        }
        s->spd_gpu[i] = mem.gpu + 32 * i;
    }
    s->valid = true;
    return true;
}

/* ===================================================================== *
 * Resource tables and FAU  [S07 6.2, 6.11, section 9 items 6-8]
 * ===================================================================== */
uint64_t
mali_build_srt(mali_arena_t *a, const air2vh_program *prog, const mali_stage_binds_t *b,
               const mali_gpu_ptr_t *t0, uint32_t t0_count)
{
    uint32_t cnt[AIR2VH_T_COUNT];
    bool any = false;
    for (unsigned i = 0; i < AIR2VH_T_COUNT; i++) {
        cnt[i] = prog->table_count[i];
        any |= cnt[i] != 0;
    }
    if (t0 && t0_count > cnt[AIR2VH_T_DRIVER]) {
        cnt[AIR2VH_T_DRIVER] = t0_count;
        any = true;
    }
    if (!any)
        return 0;

    /* The SRT register is `array | count`, count = ALIGN_POT(entries, 4) = 8 [M:vulkan/panvk_vX_cmd_desc_state.c:264-311] */
    mali_gpu_ptr_t srt = mali_arena_alloc(a, MALI_SRT_BYTES, 64);
    if (!srt.cpu)
        return 0;
    uint32_t *entry = srt.cpu;           /* arena memory is zero: unused entries stay {0,0,0} */

    for (unsigned t = 0; t < AIR2VH_T_COUNT; t++) {
        if (!cnt[t])
            continue;
        uint64_t table_gpu;
        if (t == AIR2VH_T_DRIVER) {
            if (!t0 || !t0->gpu)
                continue;
            table_gpu = t0->gpu;
        } else {
            mali_gpu_ptr_t tab = mali_arena_alloc(a, cnt[t] * MALI_DESC_SIZE, 64);
            if (!tab.cpu)
                return 0;
            uint32_t (*d)[8] = tab.cpu;
            for (uint32_t i = 0; i < cnt[t]; i++) {
                switch (t) {
                case AIR2VH_T_BUFFERS:
                    if (i < MALI_MAX_BUFFERS && b->buf_gpu[i])
                        mali_emit_buffer_desc(b->buf_gpu[i], b->buf_size[i], d[i]);
                    break;
                case AIR2VH_T_TEXTURES:
                    if (i < MALI_MAX_TEXTURES && (b->tex_mask & (1u << i)))
                        memcpy(d[i], b->tex_desc[i], 32);
                    break;
                case AIR2VH_T_SAMPLERS:
                    if (i <= MALI_MAX_SAMPLERS && (i == MALI_DUMMY_SAMPLER || (b->samp_mask & (1u << i))))
                        memcpy(d[i], b->samp_desc[i], 32);
                    break;
                case AIR2VH_T_STORAGE:
                    if (i < MALI_MAX_TEXTURES && (b->stex_mask & (1u << i)))
                        memcpy(d[i], b->stex_desc[i], 32);
                    break;
                }
            }
            table_gpu = tab.gpu;
        }
        mali_emit_resource_entry(table_gpu, (uint64_t)cnt[t] * MALI_DESC_SIZE, true, entry + 4 * t);
    }
    return srt.gpu | MALI_SRT_ENTRIES;
}

uint64_t
mali_build_fau(mali_arena_t *a, const air2vh_program *prog, const void *sysvals, size_t sysvals_size)
{
    if (!prog->fau.slots && !prog->fau.n_src && !prog->fau.n_imm)
        return 0;
    uint32_t bytes = prog->fau.slots * 8;
    if (prog->fau.n_src * 8 > bytes)
        bytes = prog->fau.n_src * 8;
    for (uint32_t i = 0; i < prog->fau.n_imm && i < AIR2VH_FAU_WORDS; i++) {
        uint32_t need = (prog->fau.imm[i].word + 1) * 4;
        if (need > bytes)
            bytes = need;
    }
    if (bytes > MALI_FAU_BYTES_MAX)
        return 0;
    if (!bytes)
        bytes = 8;
    mali_gpu_ptr_t m = mali_arena_alloc(a, MALI_ALIGN(bytes, 64), 64);
    if (!m.cpu)
        return 0;
    uint8_t *out = m.cpu;
    /* slot k <- sources[k]: 8 bytes of the sysval struct at byte_offset [S07 6.11] */
    for (uint32_t k = 0; k < prog->fau.n_src && k < AIR2VH_FAU_SLOTS; k++) {
        const uint32_t off = prog->fau.src[k].byte_offset;
        if (sysvals && (size_t)off + 8 <= sysvals_size)
            memcpy(out + 8 * k, (const uint8_t *)sysvals + off, 8);
    }
    /* then promoted immediates, by 32-bit word index (the same merge as
     * M:vulkan/panvk_vX_cmd_push_constant.c:66-74) */
    for (uint32_t i = 0; i < prog->fau.n_imm && i < AIR2VH_FAU_WORDS; i++)
        memcpy(out + 4 * prog->fau.imm[i].word, &prog->fau.imm[i].value, 4);
    return m.gpu | ((uint64_t)prog->fau.slots << 56);
}
