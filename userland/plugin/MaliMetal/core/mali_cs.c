/*
 * mali_cs.c - command-stream emission: queue init, compute dispatch, render pass
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/core/mali_cs.c
 *
 * One Metal command buffer becomes ONE command stream (a chain of 64 KiB
 * chunks built with Mesa's cs_builder) that the kernel runs through its ring
 * wrapper's CALL [S06 3.2]. The plug-in uses a single CS per queue (decision
 * D-1, MaliPluginABI.h): vertex/tiler work and the fragment job of one pass
 * run in order in that CS, separated by scoreboard waits instead of panvk's
 * cross-queue SYNC_ADD64/SYNC_WAIT64 [MV:panvk_vX_cmd_draw.c:4206-4276].
 *
 * Scoreboard plan (v11, immediate defer mode, see mali_core.h):
 *   slot 0 LS       load/store                (SB_SEL_OTHER, builder ls_sb_slot)
 *   slot 1 EP       every RUN_* / FINISH_TILING (SB_SEL_ENDPOINT)
 *   slot 2 VT       HEAP_OPERATION(VT completed) and cache flushes signal here
 *   slot 3 FRAG     FINISH_FRAGMENT signals here
 * [M:genxml/cs_builder.h:2186-2216 cs_set_state_imm32/cs_select_endpoint_sb;
 *  HWR:139-143 waits with cs_wait_slot after cs_run_compute]
 */
#include "mali_mesa.h"
#include "mali_core.h"
#include "mali_internal.h"

#include <stdlib.h>
#include <string.h>

#include "pan_encoder.h"
#include "pan_samples.h"
#include "compiler/pan_compiler.h"

#define CS_CHUNK_BYTES      (64u * 1024u)
#define R_SCRATCH           66u      /* r66..r83 scratch [MV:panvk_cmd_buffer.h PANVK_CS_REG_SCRATCH_START] */
#define MALI_USER_REGS      64u      /* shadowed staging registers r0..r63 */

struct mali_cs {
    struct cs_builder  b;
    const mali_ctx_t  *ctx;
    mali_arena_t      *arena;
    bool               ok;
    bool               has_work;
    bool               prologue;         /* REQ_RESOURCE + scoreboard state emitted   */
    bool               heap_set;         /* HEAP_SET emitted (first render pass)      */
    /* Compute+graphics share staging registers; the shadow lets a pass skip MOVEs of
     * unchanged values and is invalidated at every encoder and after every fragment job. */
    uint64_t           rv[MALI_USER_REGS];
    uint64_t           rk;               /* bit n: rv[n] is the known value of r(n) */
    /* thread storage descriptors shared by the draws of a stream */
    uint64_t           empty_tsd_gpu;
    uint64_t           tls_tsd_gpu;
    uint32_t           tls_tsd_size;
    uint64_t           sample_pos_gpu;
};

static struct cs_buffer
cs_alloc_chunk(void *cookie)
{
    struct mali_cs *cs = cookie;
    mali_gpu_ptr_t m = mali_arena_alloc(cs->arena, CS_CHUNK_BYTES, MALI_CS_STREAM_ALIGN);
    struct cs_buffer buf = {0};
    if (m.cpu) {
        buf.cpu = m.cpu;
        buf.gpu = m.gpu;
        buf.capacity = CS_CHUNK_BYTES / sizeof(uint64_t);
    }
    return buf;
}

mali_cs_t *
mali_cs_new(const mali_ctx_t *ctx, mali_arena_t *arena)
{
    struct mali_cs *cs = calloc(1, sizeof(*cs));
    if (!cs)
        return NULL;
    cs->ctx = ctx;
    cs->arena = arena;
    cs->ok = true;
    struct cs_builder_conf conf = {
        .nr_registers = (uint8_t)ctx->nr_regs,
        .nr_kernel_registers = (uint8_t)ctx->nr_kernel_regs,
        .compute_ep_limit = 0,                 /* must be 0 before v12 */
        .alloc_buffer = cs_alloc_chunk,        /* root chunk is allocated lazily */
        .cookie = cs,
        .ls_sb_slot = MALI_SB_LS,
    };
    struct cs_buffer none = {0};
    cs_builder_init(&cs->b, &conf, none);
    return cs;
}

void
mali_cs_free(mali_cs_t *cs)
{
    if (!cs)
        return;
    cs_builder_fini(&cs->b);
    free(cs);
}

bool
mali_cs_is_empty(const mali_cs_t *cs)
{
    return !cs->has_work;
}

bool
mali_cs_end(mali_cs_t *cs, uint64_t *root_va, uint32_t *size_B)
{
    struct cs_builder *b = &cs->b;
    if (!cs->has_work) {
        *root_va = 0;
        *size_B = 0;
        return cs_is_valid(b);
    }
    cs_end(b);
    if (!cs_is_valid(b) || !cs->ok)
        return false;
    *root_va = cs_root_chunk_gpu_addr(b);
    *size_B = cs_root_chunk_size(b);
    return (*root_va & (MALI_CS_STREAM_ALIGN - 1)) == 0 && (*size_B & 7) == 0 && *size_B != 0;
}

/* ---------------------------------------------------------------------- *
 * Register helpers with a shadow of the staging registers
 * ---------------------------------------------------------------------- */
static inline void
shadow_reset(struct mali_cs *cs)
{
    cs->rk = 0;
}

static inline void
set32(struct mali_cs *cs, unsigned reg, uint32_t v)
{
    if (reg < MALI_USER_REGS) {
        if ((cs->rk >> reg & 1) && cs->rv[reg] == v)
            return;
        cs->rv[reg] = v;
        cs->rk |= 1ull << reg;
    }
    cs_move32_to(&cs->b, cs_reg32(&cs->b, reg), v);
}

static inline void
set64(struct mali_cs *cs, unsigned reg, uint64_t v)
{
    if (reg + 1 < MALI_USER_REGS) {
        if ((cs->rk >> reg & 1) && (cs->rk >> (reg + 1) & 1) && cs->rv[reg] == (uint32_t)v &&
            cs->rv[reg + 1] == (uint32_t)(v >> 32))
            return;
        cs->rv[reg] = (uint32_t)v;
        cs->rv[reg + 1] = (uint32_t)(v >> 32);
        cs->rk |= 3ull << reg;
    }
    cs_move64_to(&cs->b, cs_reg64(&cs->b, reg), v);
}

/* ---------------------------------------------------------------------- *
 * Stream prologue: every stream re-asserts the state it depends on, so nothing
 * relies on CS registers surviving between command buffers (the persistence panvk
 * assumes is only [INFER]red in S06 3.4) or on which command buffer a queue runs first.
 * [MV:panvk_vX_gpu_queue.c:365-597 does this once per queue]
 * ---------------------------------------------------------------------- */
static void
ensure_prologue(struct mali_cs *cs)
{
    struct cs_builder *b = &cs->b;
    if (cs->prologue)
        return;
    cs->prologue = true;
    cs->has_work = true;
    /* Request every resource class; REQ_RESOURCE is idempotent. */
    cs_req_res(b, CS_COMPUTE_RES | CS_FRAG_RES | CS_TILER_RES | CS_IDVS_RES);
    /* Scoreboard state: endpoint tasks on slot EP, load/store on slot LS */
    cs_set_state_imm32(b, MALI_CS_SET_STATE_TYPE_SB_SEL_ENDPOINT, MALI_SB_EP);
    cs_set_state_imm32(b, MALI_CS_SET_STATE_TYPE_SB_SEL_OTHER, MALI_SB_LS);
    shadow_reset(cs);
}

/* HEAP_SET before the first render pass of the stream. The kernel's HEAP_OP(create)
 * wrote {ctx_va, first_chunk_va} into the queue-state buffer before this stream was
 * submitted (MaliPluginABI.h struct mali_heap_result). */
static void
ensure_heap(struct mali_cs *cs, uint64_t qstate_gpu)
{
    struct cs_builder *b = &cs->b;
    if (cs->heap_set)
        return;
    cs->heap_set = true;
    struct cs_index addr = cs_reg64(b, R_SCRATCH);
    struct cs_index heap = cs_reg64(b, R_SCRATCH + 2);
    cs_move64_to(b, addr, qstate_gpu + MALI_QSTATE_RESULT_OFF);
    cs_load64_to(b, heap, addr, 0);
    cs_wait_slot(b, MALI_SB_LS);
    cs_heap_set(b, heap);
}

void
mali_cs_encoder_barrier(mali_cs_t *cs)
{
    struct cs_builder *b = &cs->b;
    ensure_prologue(cs);
    /* drain everything this encoder issued, then clean the GPU caches so the next
     * encoder (or the CPU after completion) sees the data [S06 3.5; HG:336-337 flush word] */
    cs_wait_slots(b, MALI_SB_MASK(MALI_SB_EP) | MALI_SB_MASK(MALI_SB_VT) | MALI_SB_MASK(MALI_SB_FRAG) |
                         MALI_SB_MASK(MALI_SB_LS));
    struct cs_index flush_id = cs_reg32(b, R_SCRATCH);
    cs_move32_to(b, flush_id, 0);               /* latest_flush 0: unconditional [panthor_drm.h:992-1002] */
    cs_flush_caches(b, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                    MALI_CS_OTHER_FLUSH_MODE_INVALIDATE, flush_id, cs_defer(0, MALI_SB_VT));
    cs_wait_slot(b, MALI_SB_VT);
    shadow_reset(cs);
}

/* ---------------------------------------------------------------------- *
 * Thread storage descriptors
 * ---------------------------------------------------------------------- */
static void
kmod_props(const mali_ctx_t *ctx, struct pan_kmod_dev_props *kp)
{
    memset(kp, 0, sizeof(*kp));
    kp->gpu_id = ctx->props.gpu_id;
    kp->gpu_variant = ctx->props.gpu_variant;
    kp->shader_present = ctx->props.shader_present;
    kp->tiler_features = ctx->props.tiler_features;
    kp->mem_features = ctx->props.mem_features;
    kp->mmu_features = ctx->props.mmu_features;
    kp->l2_features = ctx->props.l2_features;
    kp->max_threads_per_core = ctx->props.max_threads_per_core;
    kp->max_tasks_per_core = ctx->props.max_tasks_per_core;
    kp->max_threads_per_wg = ctx->props.max_threads_per_wg;
    kp->num_registers_per_core = ctx->props.num_registers_per_core;
    kp->max_tls_instance_per_core = ctx->props.max_threads_per_core;
}

/* A LOCAL_STORAGE descriptor with TLS only (graphics), shared by all draws that
 * need no larger stack; re-created when a draw needs more TLS. [panvk_vX_cmd_draw.c:752-787
 * update_tls; panvk_vX_cmd_buffer.c:42-66 emit_tls] */
static uint64_t
graphics_tsd(struct mali_cs *cs, mali_arena_t *a, uint32_t tls_size)
{
    struct pan_tls_info info = {0};
    if (!tls_size) {
        if (!cs->empty_tsd_gpu) {
            mali_gpu_ptr_t m = mali_arena_alloc(a, MALI_LOCAL_STORAGE_LENGTH, MALI_LOCAL_STORAGE_ALIGN);
            if (!m.cpu)
                return 0;
            GENX(pan_emit_tls)(&info, (struct mali_local_storage_packed *)m.cpu);
            cs->empty_tsd_gpu = m.gpu;
        }
        return cs->empty_tsd_gpu;
    }
    if (cs->tls_tsd_gpu && cs->tls_tsd_size >= tls_size)
        return cs->tls_tsd_gpu;

    struct pan_kmod_dev_props kp;
    kmod_props(cs->ctx, &kp);
    const unsigned total = pan_get_total_stack_size(tls_size, pan_query_thread_tls_alloc(&kp), cs->ctx->core_id_range);
    mali_gpu_ptr_t tls = mali_arena_alloc(a, total, 4096);
    mali_gpu_ptr_t tsd = mali_arena_alloc(a, MALI_LOCAL_STORAGE_LENGTH, MALI_LOCAL_STORAGE_ALIGN);
    if (!tls.cpu || !tsd.cpu)
        return 0;
    info.tls.ptr = tls.gpu;
    info.tls.size = tls_size;
    GENX(pan_emit_tls)(&info, (struct mali_local_storage_packed *)tsd.cpu);
    cs->tls_tsd_gpu = tsd.gpu;
    cs->tls_tsd_size = tls_size;
    return tsd.gpu;
}

uint32_t
mali_vs_vary_size(const air2vh_vs_layout *l)
{
    if (!l || !l->blob || l->blob_size < sizeof(struct pan_varying_layout))
        return 0;
    return ((const struct pan_varying_layout *)l->blob)->generic_size_B;
}

/* ---------------------------------------------------------------------- *
 * Compute dispatch  [S06 5.3; M:vulkan/csf/panvk_vX_cmd_dispatch.c:140-340; HWR:48-146]
 * ---------------------------------------------------------------------- */
static void
task_axis_and_increment(const mali_ctx_t *ctx, const air2vh_program *prog, uint32_t threads_per_wg,
                        const uint32_t wg[3], unsigned *axis, unsigned *incr)
{
    /* M:vulkan/csf/panvk_cmd_buffer.h:869 calculate_task_axis_and_increment */
    struct pan_kmod_dev_props kp;
    kmod_props(ctx, &kp);
    const unsigned max_thread_cnt = pan_compute_max_thread_count(&kp, prog->work_reg_count[0]);
    const uint64_t total_wgs = (uint64_t)wg[0] * wg[1] * wg[2];
    const unsigned total_cores = ctx->props.core_count ? ctx->props.core_count : 1;
    *axis = MALI_TASK_AXIS_X;
    *incr = 1;
    if (!total_wgs)
        return;
    const unsigned wgs_per_core = (unsigned)((total_wgs + total_cores - 1) / total_cores);
    const unsigned threads_per_task = (max_thread_cnt + kp.max_tasks_per_core - 1) / kp.max_tasks_per_core;
    unsigned wgs_per_task = threads_per_task / (threads_per_wg ? threads_per_wg : 1);
    wgs_per_task = wgs_per_task < 1 ? 1 : (wgs_per_task > wgs_per_core ? wgs_per_core : wgs_per_task);
    *incr = wgs_per_task;
    for (unsigned i = 0; i < 2; i++) {
        if (*incr <= wg[i])
            break;
        (*axis)++;
        *incr /= wg[i];
    }
}

bool
mali_cs_dispatch(mali_cs_t *csp, const mali_ctx_t *ctx, const mali_dispatch_t *d)
{
    struct mali_cs *cs = csp;
    struct cs_builder *b = &cs->b;
    const mali_shader_t *sh = &d->pipe->cs;
    const air2vh_program *prog = &sh->prog;
    mali_arena_t *a = d->desc_arena;

    if (!sh->valid || !d->grid[0] || !d->grid[1] || !d->grid[2])
        return !!sh->valid;
    ensure_prologue(cs);

    const uint32_t threads_per_wg = d->tg_size[0] * d->tg_size[1] * d->tg_size[2];
    if (!threads_per_wg || threads_per_wg > ctx->props.max_threads_per_wg)
        return false;

    /* --- sysvals [air2vh_sysvals_cs, S07 6.11-6.12] --- */
    air2vh_sysvals_cs sv;
    memset(&sv, 0, sizeof(sv));
    for (int i = 0; i < 3; i++) {
        sv.num_work_groups[i] = d->grid[i];
        sv.local_group_size[i] = d->tg_size[i];
        sv.threads_per_grid[i] = d->threads_per_grid[i];
    }
    for (unsigned i = 0; i < MALI_MAX_BUFFERS; i++)
        sv.buffer_va[i] = d->binds->buf_gpu[i];

    /* threadgroup(n) arguments: dynamic lengths follow the static WLS, each 16-byte aligned [S07 6.4] */
    uint32_t wls_per_wg = (uint32_t)MALI_ALIGN(prog->wls_size, 16);
    for (unsigned i = 0; i < MALI_MAX_BUFFERS; i++) {
        if (!d->binds->tg_mem_len[i])
            continue;
        sv.tg_offset[i] = wls_per_wg;
        wls_per_wg += (uint32_t)MALI_ALIGN(d->binds->tg_mem_len[i], 16);
    }

    uint64_t srt = mali_build_srt(a, prog, d->binds, NULL, 0);
    uint64_t fau = mali_build_fau(a, prog, &sv, sizeof(sv));

    /* --- thread storage: TLS from the program, WLS from the dispatch --- */
    struct pan_kmod_dev_props kp;
    kmod_props(ctx, &kp);
    struct pan_tls_info tls = {.tls.size = prog->tls_size};
    if (prog->tls_size) {
        const unsigned total = pan_get_total_stack_size(prog->tls_size, pan_query_thread_tls_alloc(&kp), ctx->core_id_range);
        mali_gpu_ptr_t m = mali_arena_alloc(a, total, 4096);
        if (!m.cpu)
            return false;
        tls.tls.ptr = m.gpu;
    }
    if (wls_per_wg) {
        const struct pan_compute_dim local = {d->tg_size[0], d->tg_size[1], d->tg_size[2]};
        const struct pan_compute_dim dim = {d->grid[0], d->grid[1], d->grid[2]};
        tls.wls.size = wls_per_wg;
        tls.wls.instances = pan_calc_wls_instances(&local, &kp, &dim);
        const unsigned total = pan_calc_total_wls_size(tls.wls.size, tls.wls.instances, ctx->core_id_range);
        mali_gpu_ptr_t m = mali_arena_alloc(a, total, 4096);
        if (!m.cpu || !mali_code_window_ok(m.gpu, total))
            return false;
        tls.wls.ptr = m.gpu;
    }
    mali_gpu_ptr_t tsd = mali_arena_alloc(a, MALI_LOCAL_STORAGE_LENGTH, MALI_LOCAL_STORAGE_ALIGN);
    if (!tsd.cpu)
        return false;
    GENX(pan_emit_tls)(&tls, (struct mali_local_storage_packed *)tsd.cpu);

    /* --- staging registers: COMPUTE SR [S06 3.4] --- */
    shadow_reset(cs);
    set64(cs, MALI_COMPUTE_SR_SRT_0, srt);
    set64(cs, MALI_COMPUTE_SR_FAU_0, fau);
    set64(cs, MALI_COMPUTE_SR_SPD_0, sh->spd_gpu[0]);
    set64(cs, MALI_COMPUTE_SR_TSD_0, tsd.gpu);
    set32(cs, MALI_COMPUTE_SR_GLOBAL_ATTRIBUTE_OFFSET, 0);
    struct mali_compute_size_workgroup_packed wg_size;
    pan_pack(&wg_size, COMPUTE_SIZE_WORKGROUP, cfg) {
        cfg.workgroup_size_x = d->tg_size[0];
        cfg.workgroup_size_y = d->tg_size[1];
        cfg.workgroup_size_z = d->tg_size[2];
        cfg.allow_merging_workgroups = false;      /* v1: no merging [S07 6.12] */
    }
    set32(cs, MALI_COMPUTE_SR_WG_SIZE, wg_size.opaque[0]);
    set32(cs, MALI_COMPUTE_SR_JOB_OFFSET_X, 0);
    set32(cs, MALI_COMPUTE_SR_JOB_OFFSET_Y, 0);
    set32(cs, MALI_COMPUTE_SR_JOB_OFFSET_Z, 0);
    set32(cs, MALI_COMPUTE_SR_JOB_SIZE_X, d->grid[0]);
    set32(cs, MALI_COMPUTE_SR_JOB_SIZE_Y, d->grid[1]);
    set32(cs, MALI_COMPUTE_SR_JOB_SIZE_Z, d->grid[2]);

    unsigned axis, incr;
    task_axis_and_increment(ctx, prog, threads_per_wg, d->grid, &axis, &incr);
    cs_run_compute(b, incr, (enum mali_task_axis)axis, cs_shader_res_sel(0, 0, 0, 0));
    return cs_is_valid(b);
}

/* ---------------------------------------------------------------------- *
 * Render pass
 * ---------------------------------------------------------------------- */
struct mali_pass {
    struct mali_cs     *cs;
    const mali_ctx_t   *ctx;
    mali_arena_t       *a;
    mali_pass_desc_t    desc;
    uint32_t            width, height;
    uint64_t            tiler_ctx_gpu;
    uint64_t            fbd_reg;          /* FBD address | flags */
    bool                started;
    /* per-draw caches */
    const mali_gfx_pipeline_t *last_pipe;
    float               last_blend_color[4];
    uint64_t            bds_reg;
    uint64_t            blend_descs[MALI_MAX_RTS];
    bool                any_dest_read;
    bool                blend_valid;
    /* descriptor reuse between draws: a Metal pass issues many draws with unchanged bindings */
    struct { const air2vh_program *prog; const mali_stage_binds_t *b; uint64_t serial, t0; uint64_t val; bool valid; } srt_c[2];
    struct { const air2vh_program *prog; uint8_t blob[sizeof(air2vh_sysvals_gfx)]; uint64_t val; bool valid; } fau_c[2];
    struct { uint8_t key[160]; uint64_t val; bool valid; } zsd_c;
};

static enum pipe_format
att_format(const mali_attachment_t *at)
{
    return at->tex ? (enum pipe_format)mali_texture_pipe_format(at->tex) : PIPE_FORMAT_NONE;
}

/* A single-subresource colour/zs view of an attachment. */
static void
att_view(const mali_attachment_t *at, struct pan_image_view *out)
{
    *out = *mali_texture_pan_view(at->tex);
    out->dim = MALI_TEXTURE_DIMENSION_2D;
    out->first_level = out->last_level = out->first_level + at->level;
    out->first_layer_or_z_slice = out->last_layer_or_z_slice = out->first_layer_or_z_slice + at->slice;
}

static void
fill_clear_color(enum pipe_format fmt, const float c[4], union pipe_color_union *u)
{
    memset(u, 0, sizeof(*u));
    if (util_format_is_pure_uint(fmt)) {
        for (int i = 0; i < 4; i++) u->ui[i] = (uint32_t)(c[i] < 0 ? 0 : c[i]);
    } else if (util_format_is_pure_sint(fmt)) {
        for (int i = 0; i < 4; i++) u->i[i] = (int32_t)c[i];
    } else {
        for (int i = 0; i < 4; i++) u->f[i] = c[i];
    }
}

mali_pass_t *
mali_pass_begin(mali_cs_t *csp, const mali_ctx_t *ctx, const mali_pass_desc_t *d)
{
    struct mali_cs *cs = csp;
    struct cs_builder *b = &cs->b;
    mali_arena_t *a = d->desc_arena;

    if (!d->width || !d->height || d->width > 65535 || d->height > 65535 || d->sample_count > 1 || d->layers > 1)
        return NULL;

    /* ---- framebuffer layout [M:lib/pan_fb.h struct pan_fb_layout; MV:panvk_vX_cmd_draw.c get_fb_descs] ---- */
    struct pan_fb_layout fb;
    memset(&fb, 0, sizeof(fb));
    fb.width_px = d->width;
    fb.height_px = d->height;
    fb.render_area_px = pan_fb_bbox_from_xywh(0, 0, d->width, d->height);
    fb.tiling_area_px = fb.render_area_px;
    fb.sample_count = 1;
    fb.rt_count = d->color_count ? d->color_count : 1;   /* the hardware needs >= 1 RT even if a dummy */
    for (unsigned i = 0; i < PAN_MAX_RTS; i++)
        fb.rt_formats[i] = i < d->color_count ? att_format(&d->color[i]) : PIPE_FORMAT_NONE;
    if (d->depth.tex)
        fb.z_format = util_format_get_depth_only(att_format(&d->depth));
    if (d->stencil.tex)
        fb.s_format = util_format_has_stencil(util_format_description(att_format(&d->stencil)))
                          ? PIPE_FORMAT_S8_UINT : PIPE_FORMAT_NONE;
    fb.tile_rt_budget_B = ctx->tile_rt_budget_B;
    fb.tile_z_budget_B = ctx->tile_z_budget_B;
    GENX(pan_select_fb_tile_size)(&fb);

    /* ---- load / store ---- */
    struct pan_image_view rt_views[PAN_MAX_RTS], z_view, s_view;
    struct pan_fb_load load;
    struct pan_fb_store store;
    memset(&load, 0, sizeof(load));
    memset(&store, 0, sizeof(store));
    for (unsigned i = 0; i < d->color_count; i++) {
        const mali_attachment_t *at = &d->color[i];
        if (!at->tex)
            continue;
        att_view(at, &rt_views[i]);
        store.rts[i].iview = &rt_views[i];
        store.rts[i].store = at->store == MALI_STORE_STORE;
        store.rts[i].msaa = PAN_FB_MSAA_COPY_ALL;
        if (at->load == MALI_LOAD_CLEAR) {
            load.rts[i].in_bounds_load = load.rts[i].border_load = PAN_FB_LOAD_CLEAR;
            load.rts[i].always = true;           /* clears write even tiles with no primitives */
            fill_clear_color(fb.rt_formats[i], at->clear_color, &load.rts[i].clear.color);
        }
        /* LoadActionLoad is a fullscreen draw issued by the encoder (MaliRenderEncoder.m);
         * the FBD preload path would need a frame shader built from NIR. */
    }
    if (d->depth.tex) {
        att_view(&d->depth, &z_view);
        store.zs.iview = &z_view;
        store.zs.store = d->depth.store == MALI_STORE_STORE;
        store.zs.msaa = PAN_FB_MSAA_COPY_ALL;
        if (d->depth.load == MALI_LOAD_CLEAR) {
            load.z.in_bounds_load = load.z.border_load = PAN_FB_LOAD_CLEAR;
            load.z.always = true;
            load.z.clear.depth = (float)d->depth.clear_depth;
        }
    }
    if (d->stencil.tex) {
        att_view(&d->stencil, &s_view);
        store.s.iview = &s_view;
        store.s.store = d->stencil.store == MALI_STORE_STORE;
        store.s.msaa = PAN_FB_MSAA_COPY_ALL;
        if (d->stencil.load == MALI_LOAD_CLEAR) {
            load.s.in_bounds_load = load.s.border_load = PAN_FB_LOAD_CLEAR;
            load.s.always = true;
            load.s.clear.stencil = (uint8_t)d->stencil.clear_stencil;
        }
    }
    GENX(pan_align_fb_tiling_area)(&fb, &store);

    /* ---- tiler context + sample positions ---- */
    uint64_t tiler_gpu;
    if (!mali_state_emit_tiler_ctx(a, ctx, d->heap_desc_gpu, d->width, d->height, fb.tile_size_px, &tiler_gpu))
        return NULL;
    if (!cs->sample_pos_gpu) {
        const unsigned sz = pan_sample_positions_buffer_size();
        mali_gpu_ptr_t sp = mali_arena_alloc(a, sz, 64);
        if (!sp.cpu)
            return NULL;
        pan_upload_sample_positions(sp.cpu);
        cs->sample_pos_gpu = sp.gpu;
    }

    /* ---- framebuffer descriptor [MV:panvk_vX_cmd_draw.c:1759-1812] ---- */
    struct pan_tiler_context tiler = {.valhall = {.desc = tiler_gpu, .layer_offset = 0}};
    struct pan_fb_desc_info info = {
        .fb = &fb,
        .load = &load,
        .store = &store,
        .sample_pos_array_pointer = cs->sample_pos_gpu + pan_sample_positions_offset(pan_sample_pattern(1)),
        .provoking_vertex_first = true,
        .tiler_ctx = &tiler,
        .layer = 0,
    };
    const bool zs_ext = GENX(pan_fb_needs_zs_crc_ext)(&info);
    const uint32_t fb_sz = MALI_FRAMEBUFFER_LENGTH;
    const uint32_t fbd_sz = fb_sz + (zs_ext ? MALI_ZS_CRC_EXTENSION_LENGTH : 0) + fb.rt_count * MALI_RENDER_TARGET_LENGTH;
    mali_gpu_ptr_t fbd = mali_arena_alloc(a, fbd_sz, MALI_FRAMEBUFFER_ALIGN);
    if (!fbd.cpu)
        return NULL;
    const struct pan_fb_descs descs = {
        .fbd = fbd.cpu,
        .zs_crc = zs_ext ? (void *)((uint8_t *)fbd.cpu + fb_sz) : NULL,
        .rts = (void *)((uint8_t *)fbd.cpu + fb_sz + (zs_ext ? MALI_ZS_CRC_EXTENSION_LENGTH : 0)),
    };
    const uint32_t fbd_flags = GENX(pan_emit_fb_desc)(&info, &descs);

    struct mali_pass *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->cs = cs;
    p->ctx = ctx;
    p->a = a;
    p->desc = *d;
    p->width = d->width;
    p->height = d->height;
    p->tiler_ctx_gpu = tiler_gpu;
    p->fbd_reg = fbd.gpu | fbd_flags;

    /* ---- the tiler context was written by the CPU, so the VT start marker follows
     * directly [MV:panvk_vX_cmd_draw.c:1426 cs_vt_start(b, cs_now())] ---- */
    ensure_prologue(cs);
    ensure_heap(cs, d->heap_desc_gpu);
    shadow_reset(cs);
    set64(cs, MALI_IDVS_SR_TILER_CTX, tiler_gpu);
    cs_vt_start(b, cs_now());
    p->started = true;
    return p;
}

void
mali_pass_abort(mali_pass_t *p)
{
    free(p);
}

static inline uint32_t
clampi(int v, int lo, int hi)
{
    return (uint32_t)(v < lo ? lo : (v > hi ? hi : v));
}


/* SRT for stage `st` (0 VS, 1 FS), reused while bindings, program and T0 are unchanged. */
static uint64_t
pass_srt(struct mali_pass *p, mali_arena_t *a, int st, const air2vh_program *prog, const mali_stage_binds_t *b,
         const mali_gpu_ptr_t *t0, uint32_t t0_count)
{
    typeof(p->srt_c[0]) *c = &p->srt_c[st];
    const uint64_t t0g = t0 ? t0->gpu : 0;
    if (b->serial && c->valid && c->prog == prog && c->b == b && c->serial == b->serial && c->t0 == t0g)
        return c->val;
    const uint64_t v = mali_build_srt(a, prog, b, t0, t0_count);
    *c = (typeof(*c)){prog, b, b->serial, t0g, v, true};
    return v;
}

static uint64_t
pass_fau(struct mali_pass *p, mali_arena_t *a, int st, const air2vh_program *prog, const air2vh_sysvals_gfx *sv)
{
    typeof(p->fau_c[0]) *c = &p->fau_c[st];
    if (c->valid && c->prog == prog && !memcmp(c->blob, sv, sizeof(*sv)))
        return c->val;
    const uint64_t v = mali_build_fau(a, prog, sv, sizeof(*sv));
    c->prog = prog;
    memcpy(c->blob, sv, sizeof(*sv));
    c->val = v;
    c->valid = true;
    return v;
}

bool
mali_pass_draw(mali_pass_t *p, const mali_draw_state_t *s)
{
    struct mali_cs *cs = p->cs;
    struct cs_builder *b = &cs->b;
    const mali_gfx_pipeline_t *pipe = s->pipe;
    mali_arena_t *a = s->desc_arena ? s->desc_arena : p->a;
    if (!pipe || !pipe->vs.valid || !s->vs_binds)
        return false;
    if (!s->count || !s->instance_count)
        return true;                            /* nothing to draw */
    const bool has_fs = pipe->fs.valid && s->fs_binds;
    const air2vh_program *vp = &pipe->vs.prog;
    const air2vh_program *fp = &pipe->fs.prog;

    /* ---- blend descriptors (rebuilt only when the pipeline or constants change) ---- */
    if (!p->blend_valid || p->last_pipe != pipe || memcmp(p->last_blend_color, s->blend_color, sizeof(float) * 4)) {
        if (!mali_state_emit_blend(a, pipe, s->blend_color, &p->bds_reg, p->blend_descs, &p->any_dest_read))
            return false;
        p->blend_valid = true;
        p->last_pipe = pipe;
        memcpy(p->last_blend_color, s->blend_color, sizeof(float) * 4);
    }

    /* ---- viewport, scissor, depth range ---- */
    float vx = 0, vy = 0, vw = (float)p->width, vh = (float)p->height, zn = 0, zf = 1;
    if (s->viewport_set) {
        vx = s->viewport.x; vy = s->viewport.y; vw = s->viewport.w; vh = s->viewport.h;
        zn = s->viewport.znear; zf = s->viewport.zfar;
    }
    /* NDC y is up in Metal, framebuffer y is down: scale.y is negative [S07 6.11 + Metal convention] */
    air2vh_sysvals_gfx svv, svf;
    memset(&svv, 0, sizeof(svv));
    svv.viewport_scale.x = 0.5f * vw;
    svv.viewport_scale.y = -0.5f * vh;
    svv.viewport_scale.z = zf - zn;
    svv.viewport_offset.x = vx + 0.5f * vw;
    svv.viewport_offset.y = vy + 0.5f * vh;
    svv.viewport_offset.z = zn;
    svv.first_vertex = s->indexed ? s->base_vertex : (int32_t)s->vertex_start;
    svv.base_instance = (int32_t)s->base_instance;
    for (unsigned i = 0; i < MALI_MAX_BUFFERS; i++)
        svv.buffer_va[i] = s->vs_binds->buf_gpu[i];
    svf = svv;
    for (unsigned i = 0; i < MALI_MAX_BUFFERS; i++)
        svf.buffer_va[i] = s->fs_binds ? s->fs_binds->buf_gpu[i] : 0;
    for (int i = 0; i < 4; i++)
        svf.blend.constants[i] = s->blend_color[i];     /* FS words 0..3 [S07 6.11] */
    memcpy(svf.blend_descs, p->blend_descs, sizeof(svf.blend_descs));
    memcpy(svv.blend_descs, p->blend_descs, sizeof(svv.blend_descs));

    int minx = (int)vx, maxx = (int)(vx + vw);
    int miny = (int)(vh < 0 ? vy + vh : vy), maxy = (int)(vh < 0 ? vy : vy + vh);
    if (s->scissor_set) {
        if ((int)s->scissor.x > minx) minx = (int)s->scissor.x;
        if ((int)s->scissor.y > miny) miny = (int)s->scissor.y;
        if ((int)(s->scissor.x + s->scissor.w) < maxx) maxx = (int)(s->scissor.x + s->scissor.w);
        if ((int)(s->scissor.y + s->scissor.h) < maxy) maxy = (int)(s->scissor.y + s->scissor.h);
    }
    maxx = maxx > minx ? maxx - 1 : maxx;
    maxy = maxy > miny ? maxy - 1 : maxy;
    struct mali_scissor_packed sc;
    pan_pack(&sc, SCISSOR, cfg) {
        cfg.scissor_minimum_x = clampi(minx, 0, 65535);
        cfg.scissor_minimum_y = clampi(miny, 0, 65535);
        cfg.scissor_maximum_x = clampi(maxx, 0, 65535);
        cfg.scissor_maximum_y = clampi(maxy, 0, 65535);
    }
    uint64_t scissor_reg;
    memcpy(&scissor_reg, &sc, sizeof(scissor_reg));

    /* ---- FAU, SRT, TSD, ZSD ---- */
    const uint64_t vs_fau = pass_fau(p, a, 0, vp, &svv);
    const uint64_t fs_fau = has_fs ? pass_fau(p, a, 1, fp, &svf) : 0;
    const uint64_t vs_srt = pass_srt(p, a, 0, vp, s->vs_binds, &s->t0_vs, s->t0_vs_count);
    const uint64_t fs_srt = has_fs ? pass_srt(p, a, 1, fp, s->fs_binds, NULL, 0) : 0;
    const uint32_t tls_size = vp->tls_size > (has_fs ? fp->tls_size : 0) ? vp->tls_size : (has_fs ? fp->tls_size : 0);
    const uint64_t tsd = graphics_tsd(cs, a, tls_size);
    /* ZSD depends on a few scalars only: reuse the descriptor while they are unchanged */
    struct zsd_key {
        const mali_gfx_pipeline_t *pipe; uint32_t zs[16]; uint32_t has_zs; uint32_t sref[2]; uint32_t clip;
        float bias[3];
    } key;
    memset(&key, 0, sizeof(key));
    key.pipe = pipe;
    key.has_zs = s->zs != NULL;
    if (s->zs) {
        key.zs[0] = s->zs->depth_compare; key.zs[1] = s->zs->depth_write; key.zs[2] = s->zs->stencil_enable;
        memcpy(&key.zs[3], &s->zs->front, sizeof(s->zs->front));
        memcpy(&key.zs[9], &s->zs->back, sizeof(s->zs->back));
    }
    key.sref[0] = s->stencil_ref_front; key.sref[1] = s->stencil_ref_back; key.clip = s->depth_clip_mode;
    key.bias[0] = s->depth_bias; key.bias[1] = s->depth_slope; key.bias[2] = s->depth_clamp;
    _Static_assert(sizeof(key) <= sizeof(p->zsd_c.key), "zsd cache key");
    uint64_t zsd;
    if (!tsd)
        return false;
    if (p->zsd_c.valid && !memcmp(p->zsd_c.key, &key, sizeof(key))) {
        zsd = p->zsd_c.val;
    } else {
        if (!mali_state_emit_zsd(a, pipe, s, &zsd))
            return false;
        memset(p->zsd_c.key, 0, sizeof(p->zsd_c.key));
        memcpy(p->zsd_c.key, &key, sizeof(key));
        p->zsd_c.val = zsd;
        p->zsd_c.valid = true;
    }
    if ((vp->fau.slots && !vs_fau) || (has_fs && fp->fau.slots && !fs_fau))
        return false;

    uint32_t dcd[3], pflags, pflags2, override;
    mali_state_dcd(pipe, s, p->any_dest_read, dcd, NULL);
    mali_state_prim_flags(pipe, s, &pflags, &pflags2, &override);

    /* ---- IDVS staging registers [S06 5.4 table; MV:panvk_vX_cmd_draw.c:2160-3175] ---- */
    set64(cs, MALI_IDVS_SR_VERTEX_SRT, vs_srt);
    set64(cs, MALI_IDVS_SR_FRAGMENT_SRT, fs_srt);
    set64(cs, MALI_IDVS_SR_VERTEX_FAU, vs_fau);
    set64(cs, MALI_IDVS_SR_FRAGMENT_FAU, fs_fau);
    /* position SPD: points use entry 0, everything else the no-point-size variant (entry 1) */
    set64(cs, MALI_IDVS_SR_VERTEX_POS_SPD, s->prim == 0 ? pipe->vs.spd_gpu[0] : pipe->vs.spd_gpu[1]);
    set64(cs, MALI_IDVS_SR_VERTEX_VARY_SPD, vp->vs.secondary_enable ? pipe->vs.spd_gpu[2] : 0);
    set64(cs, MALI_IDVS_SR_FRAGMENT_SPD, has_fs ? pipe->fs.spd_gpu[0] : 0);
    set64(cs, MALI_IDVS_SR_TSD_0, tsd);
    set32(cs, MALI_IDVS_SR_GLOBAL_ATTRIBUTE_OFFSET, 0);
    set32(cs, MALI_IDVS_SR_INDEX_COUNT, s->count);
    set32(cs, MALI_IDVS_SR_INSTANCE_COUNT, s->instance_count);
    set32(cs, MALI_IDVS_SR_INDEX_OFFSET, 0);
    set32(cs, MALI_IDVS_SR_VERTEX_OFFSET, s->indexed ? (uint32_t)s->base_vertex : s->vertex_start);
    set32(cs, MALI_IDVS_SR_INSTANCE_OFFSET, 0);   /* NIR wants a zero-based instance id [panvk_vX_cmd_draw.c:3097-3108] */
    if (s->indexed) {
        set32(cs, MALI_IDVS_SR_INDEX_BUFFER_SIZE, s->index_buffer_size);
        set64(cs, MALI_IDVS_SR_INDEX_BUFFER, s->index_gpu);
    }
    set64(cs, MALI_IDVS_SR_TILER_CTX, p->tiler_ctx_gpu);
    set32(cs, MALI_IDVS_SR_TILER_FLAGS, pflags);
    set32(cs, MALI_IDVS_SR_TILER_FLAGS2, pflags2);
    set32(cs, MALI_IDVS_SR_VARY_SIZE, pipe->vs.vary_size_B);
    set32(cs, MALI_IDVS_SR_DCD0, dcd[0]);
    set32(cs, MALI_IDVS_SR_DCD1, dcd[1]);
    set32(cs, MALI_IDVS_SR_DCD2, dcd[2]);
    set64(cs, MALI_IDVS_SR_ZSD, zsd);
    set64(cs, MALI_IDVS_SR_BLEND_DESC, p->bds_reg);
    set64(cs, MALI_IDVS_SR_SCISSOR_BOX, scissor_reg);
    set32(cs, MALI_IDVS_SR_LOW_DEPTH_CLAMP, mali_f2u(zn < zf ? zn : zf));
    set32(cs, MALI_IDVS_SR_HIGH_DEPTH_CLAMP, mali_f2u(zn < zf ? zf : zn));
    set64(cs, MALI_IDVS_SR_OQ, 0);
    if (s->prim <= 2) {      /* points and lines: fixed size/width 1.0 [panvk_vX_cmd_draw.c:1014-1065] */
        set32(cs, MALI_IDVS_SR_PRIMITIVE_SIZE, mali_f2u(1.0f));
    }

    cs_run_idvs(b, override, true /* malloc IDVS [S07 6.9] */, cs_shader_res_sel(0, 0, 1, 0),
                cs_shader_res_sel(2, 2, 2, 0), cs_undef());
    return cs_is_valid(b);
}

bool
mali_pass_end(mali_pass_t *p)
{
    struct mali_cs *cs = p->cs;
    struct cs_builder *b = &cs->b;
    bool ok = true;

    /* ---- vertex/tiler completion [MV:panvk_vX_cmd_draw.c:4195-4256] ---- */
    cs_finish_tiling(b);
    cs_vt_end(b, cs_defer(MALI_SB_MASK(MALI_SB_EP), MALI_SB_VT));
    cs_wait_slot(b, MALI_SB_VT);                 /* the fragment job may start only after tiling ended */

    /* ---- fragment job [MV:panvk_vX_cmd_draw.c:4478-4690] ---- */
    set32(cs, MALI_FRAGMENT_SR_BBOX_MIN, (uint32_t)0);                       /* min_x | min_y << 16 */
    set32(cs, MALI_FRAGMENT_SR_BBOX_MAX, (p->width - 1) | ((p->height - 1) << 16));
    set64(cs, MALI_FRAGMENT_SR_FBD_POINTER, p->fbd_reg);
    cs_run_fragment(b, false, MALI_TILE_RENDER_ORDER_Z_ORDER);

    /* completed top/bottom (chunks the fragment job frees) live at offset 40 of the tiler
     * context, written when tiling ended [MV:panvk_vX_cmd_draw.c:4640-4660] */
    struct cs_index tiler = cs_reg64(b, R_SCRATCH);
    struct cs_index completed = cs_reg_tuple(b, R_SCRATCH + 2, 4);
    struct cs_index top = cs_reg64(b, R_SCRATCH + 2);
    struct cs_index bottom = cs_reg64(b, R_SCRATCH + 4);
    cs_move64_to(b, tiler, p->tiler_ctx_gpu);
    cs_load_to(b, completed, tiler, BITFIELD_MASK(4), 40);
    cs_wait_slot(b, MALI_SB_LS);
    cs_finish_fragment(b, true, top, bottom, cs_defer(MALI_SB_MASK(MALI_SB_EP), MALI_SB_FRAG));
    cs_wait_slot(b, MALI_SB_FRAG);

    ok = cs_is_valid(b);
    shadow_reset(cs);        /* RUN_FRAGMENT rewrites r0..r43 [MV:panvk_cmd_buffer.h RANGE_0] */
    free(p);
    return ok;
}
