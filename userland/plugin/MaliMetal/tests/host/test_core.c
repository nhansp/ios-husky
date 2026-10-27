/*
 * test_core.c - host unit tests for the MaliMetal core (no GPU, no Metal)
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/tests/host/test_core.c
 *
 * Builds real command streams with Mesa's cs_builder, then decodes them with the
 * generated genxml unpackers and replays the MOVE instructions into a register
 * file, so each test can assert "at RUN_COMPUTE the staging registers hold X".
 * This validates the Mesa-facing half of the plug-in (the half that decides
 * whether the GPU hangs); the Objective-C half is exercised on a device
 * (tests/device in the spec 00 plan).
 *
 * Build: tests/host/build_host.sh
 */
#include "mali_mesa.h"       /* first */
#include "mali_core.h"
#include "mali_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...)                                                          \
    do {                                                                       \
        if (!(c)) {                                                            \
            printf("FAIL %s:%d: %s  ", __FILE__, __LINE__, #c);                \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

/* ---- fake GPU memory: chunks are host memory with a fake 48-bit GPU VA ---- */
static uint64_t g_next_va = 0x0000000200000000ull;
static bool
test_new_chunk(void *ctx, uint32_t min, mali_gpu_ptr_t *out, uint32_t *size)
{
    (void)ctx;
    uint32_t sz = min > (1u << 20) ? (uint32_t)MALI_ALIGN(min, 4096) : (1u << 20);
    void *p = NULL;
    if (posix_memalign(&p, 4096, sz))
        return false;
    memset(p, 0, sz);
    out->cpu = p;
    out->gpu = g_next_va;
    g_next_va += MALI_ALIGN(sz, 1u << 20);
    *size = sz;
    return true;
}
static const mali_arena_ops_t test_ops = {.new_chunk = test_new_chunk};

/* find a host pointer for a fake GPU address by scanning the arena chunks we handed out */
struct chunk { void *cpu; uint64_t gpu; uint32_t size; };
static struct chunk g_chunks[64];
static unsigned g_nchunks;
static bool
rec_new_chunk(void *ctx, uint32_t min, mali_gpu_ptr_t *out, uint32_t *size)
{
    if (!test_new_chunk(ctx, min, out, size))
        return false;
    g_chunks[g_nchunks++] = (struct chunk){out->cpu, out->gpu, *size};
    return true;
}
static const mali_arena_ops_t rec_ops = {.new_chunk = rec_new_chunk};
static void *
host_of(uint64_t gpu)
{
    for (unsigned i = 0; i < g_nchunks; i++)
        if (gpu >= g_chunks[i].gpu && gpu < g_chunks[i].gpu + g_chunks[i].size)
            return (uint8_t *)g_chunks[i].cpu + (gpu - g_chunks[i].gpu);
    return NULL;
}


/* ---- stream decoding ---- */
struct regfile { uint32_t r[96]; };
static void
decode_stream(const uint64_t *ins, unsigned n, struct regfile *rf, int *counts, void (*on_run)(unsigned op, const struct regfile *, void *), void *u)
{
    for (unsigned i = 0; i < n; i++) {
        uint64_t w = ins[i];
        unsigned op = (unsigned)(w >> 56);
        counts[op]++;
        switch (op) {
        case MALI_CS_OPCODE_MOVE48: {
            const unsigned dst = (unsigned)(w >> 48) & 0xff;      /* [M:genxml/v11.xml CS MOVE48] */
            rf->r[dst] = (uint32_t)w;
            rf->r[dst + 1] = (uint32_t)((w >> 32) & 0xffff);
            break;
        }
        case MALI_CS_OPCODE_MOVE32: {
            rf->r[(unsigned)(w >> 48) & 0xff] = (uint32_t)w;
            break;
        }
        case MALI_CS_OPCODE_RUN_COMPUTE:
        case MALI_CS_OPCODE_RUN_IDVS:
        case MALI_CS_OPCODE_RUN_FRAGMENT:
            if (on_run)
                on_run(op, rf, u);
            break;
        default:
            break;
        }
    }
}
static uint64_t R64(const struct regfile *rf, unsigned r) { return (uint64_t)rf->r[r] | ((uint64_t)rf->r[r + 1] << 32); }

/* ---- fake program ---- */
static void
fake_program(air2vh_program *p, unsigned stage)
{
    memset(p, 0, sizeof(*p));
    p->struct_size = sizeof(*p);
    p->abi_version = 1;
    p->stage = stage;
    p->code_size = 256;
    p->spd_count = stage == 1 ? 3 : 1;
    p->work_reg_count[0] = p->work_reg_count[1] = p->work_reg_count[2] = 32;
    p->entry_offset[1] = 64;
    p->entry_offset[2] = 128;
    p->fau.slots = 3;
    p->fau.n_src = 2;
    p->fau.src[0].byte_offset = offsetof(air2vh_sysvals_gfx, viewport_scale);
    p->fau.src[1].byte_offset = offsetof(air2vh_sysvals_gfx, buffer_va);
    p->fau.n_imm = 1;
    p->fau.imm[0].word = 4;
    p->fau.imm[0].value = 0x12345678u;
    p->table_count[AIR2VH_T_BUFFERS] = 2;
    p->table_count[AIR2VH_T_TEXTURES] = 1;
    p->table_count[AIR2VH_T_SAMPLERS] = 17;
    if (stage == 1) {
        p->vs.secondary_enable = 1;
        p->vs.idvs = 1;
    }
    if (stage == 2) {
        p->fs.rt[0].index = 0;
        p->fs.rt[0].comps = 4;
        p->fs.rt[0].written = 1;
        p->fs.can_fpk = 1;
        /* LUT: update=0 (force early), kill=0 */
    }
}
static void
upload_shader(mali_shader_t *sh, mali_arena_t *a, unsigned stage, uint64_t code)
{
    memset(sh, 0, sizeof(*sh));
    fake_program(&sh->prog, stage);
    sh->code_gpu = code;
    mali_gpu_ptr_t spd = mali_arena_alloc(a, MALI_SPD_MEM_BYTES, 32);
    CHECK(mali_shader_build_spds(sh, spd), "spd build");
}

static void
fill_binds(mali_stage_binds_t *b, mali_arena_t *a, const mali_texture_t *tex)
{
    memset(b, 0, sizeof(*b));
    b->buf_gpu[0] = 0x100000000ull + 0x40;
    b->buf_size[0] = 4096;
    b->buf_gpu[1] = 0x100010000ull;
    b->buf_size[1] = 256;
    if (tex) {
        CHECK(mali_emit_sampled_texture(tex, a, b->tex_desc[0]), "tex desc");
        b->tex_mask = 1;
    }
    mali_sampler_params_t sp = {.min_filter = 1, .mag_filter = 1, .mip_filter = 0, .addr_s = 0, .addr_t = 0,
                                .addr_r = 0, .border = 0, .max_anisotropy = 1, .compare = 8,
                                .normalized_coords = true, .lod_min = 0, .lod_max = 1000.0f};
    mali_emit_sampler(&sp, false, b->samp_desc[0]);
    mali_emit_sampler(&sp, false, b->samp_desc[MALI_DUMMY_SAMPLER]);
    b->samp_mask = 1;
}

/* ===================================================================== */
static void
test_formats(void)
{
    mali_fmt_info_t f;
    CHECK(mali_fmt_lookup(80, &f) && f.bytes_per_pixel == 4 && (f.flags & MALI_FMT_COLOR_RT), "BGRA8");
    CHECK(f.pipe_format == PIPE_FORMAT_B8G8R8A8_UNORM, "BGRA8 pipe");
    CHECK(mali_fmt_lookup(70, &f) && f.pipe_format == PIPE_FORMAT_R8G8B8A8_UNORM, "RGBA8");
    CHECK(mali_fmt_lookup(10, &f) && f.bytes_per_pixel == 1, "R8");
    CHECK(mali_fmt_lookup(1, &f) && f.pipe_format == PIPE_FORMAT_A8_UNORM, "A8");
    CHECK(!mali_fmt_lookup(554, &f), "XR formats are refused");
    CHECK(!mali_fmt_lookup(500, &f), "YCbCr formats are refused");
    CHECK(mali_vertex_fmt_to_pipe(31) == PIPE_FORMAT_R32G32B32A32_FLOAT, "Float4");
}

static void
test_texture(const mali_ctx_t *ctx)
{
    mali_texture_params_t p = {.mtl_pixel_format = 80, .mtl_texture_type = MALI_TEX_2D, .width = 256, .height = 128,
                               .depth = 1, .array_length = 1, .mip_levels = 1, .sample_count = 1};
    uint64_t sz;
    mali_texture_t *t = mali_texture_new(ctx, &p, &sz);
    CHECK(t != NULL, "texture");
    if (!t)
        return;
    mali_subresource_t sr;
    CHECK(mali_texture_subresource(t, 0, 0, &sr), "subres");
    CHECK(sr.row_stride_B == 1024 && sr.offset_B == 0, "row %u off %llu", sr.row_stride_B, (unsigned long long)sr.offset_B);
    CHECK(sz >= 1024ull * 128, "size %llu", (unsigned long long)sz);
    mali_texture_free(t);

    /* IOSurface-style explicit pitch */
    p.explicit_row_pitch = 1152;
    t = mali_texture_new(ctx, &p, &sz);
    CHECK(t && mali_texture_subresource(t, 0, 0, &sr) && sr.row_stride_B == 1152, "explicit pitch 1152");
    mali_texture_free(t);
    p.explicit_row_pitch = 1000;       /* not 64-byte aligned: Mesa refuses, the plug-in shadows it */
    t = mali_texture_new(ctx, &p, &sz);
    CHECK(t == NULL, "pitch 1000 refused");
    p.explicit_row_pitch = 0;

    /* mip chain */
    p.mip_levels = 4;
    t = mali_texture_new(ctx, &p, &sz);
    CHECK(t != NULL, "mips");
    if (t) {
        mali_subresource_t s1;
        CHECK(mali_texture_subresource(t, 1, 0, &s1) && s1.width == 128 && s1.height == 64 && s1.offset_B >= 1024ull * 128,
              "level1 %ux%u off %llu", s1.width, s1.height, (unsigned long long)s1.offset_B);
        mali_texture_set_base(t, 0x300000000ull);
        mali_arena_t a;
        mali_arena_init(&a, &test_ops, NULL);
        uint32_t d[8];
        CHECK(mali_emit_sampled_texture(t, &a, d), "emit sampled");
        pan_cast_and_unpack(d, TEXTURE, tx);
        CHECK(tx.width == 256 && tx.height == 128 && tx.levels == 4, "TEXTURE %ux%u levels %u", tx.width, tx.height, tx.levels);
        CHECK(tx.surfaces >= a.gpu && tx.surfaces < a.gpu + a.size, "payload address inside the arena");
        mali_texture_free(t);
    }

    /* view */
    p.mip_levels = 1;
    mali_texture_t *par = mali_texture_new(ctx, &p, &sz);
    mali_texture_t *v = par ? mali_texture_view(par, 70, MALI_TEX_2D, 0, 1, 0, 1) : NULL;
    CHECK(v != NULL, "RGBA8 view of BGRA8");
    CHECK(par && !mali_texture_view(par, 25, MALI_TEX_2D, 0, 1, 0, 1), "R16Float view of BGRA8 is refused (size differs)");
    mali_texture_free(v);
    mali_texture_free(par);
}

static void
test_sampler(void)
{
    mali_sampler_params_t sp = {.min_filter = 0, .mag_filter = 1, .mip_filter = 2, .addr_s = 2, .addr_t = 3, .addr_r = 0,
                                .border = 2, .max_anisotropy = 4, .compare = 1, .normalized_coords = true,
                                .lod_min = 1.0f, .lod_max = 5.0f, .lod_bias = 0.5f};
    uint32_t d[8];
    mali_emit_sampler(&sp, false, d);
    pan_cast_and_unpack(d, SAMPLER, s);
    CHECK(s.minify_nearest && !s.magnify_nearest, "filters");
    CHECK(s.wrap_mode_s == MALI_WRAP_MODE_REPEAT && s.wrap_mode_t == MALI_WRAP_MODE_MIRRORED_REPEAT, "wrap");
    CHECK(s.mipmap_mode == MALI_MIPMAP_MODE_PERFORMANCE_TRILINEAR, "aniso -> performance trilinear");
    CHECK(s.maximum_anisotropy == 4 && s.lod_algorithm == MALI_LOD_ALGORITHM_ANISOTROPIC, "aniso");
    CHECK(s.compare_function == MALI_FUNC_GREATER, "less flipped to greater, got %d", (int)s.compare_function);
    CHECK(s.border_color_a == 0x3f800000u, "opaque white border float bits");
    mali_emit_sampler(&sp, true, d);
    pan_cast_and_unpack(d, SAMPLER, s_);
    CHECK(s_.border_color_a == 1u, "integer border bits");
}

/* ---- compute ---- */
struct run_ctx { unsigned runs; uint32_t job_size[3]; uint64_t srt, fau, spd, tsd; uint32_t wg; };
static void
on_run_compute(unsigned op, const struct regfile *rf, void *u)
{
    struct run_ctx *c = u;
    if (op != MALI_CS_OPCODE_RUN_COMPUTE)
        return;
    c->runs++;
    c->job_size[0] = rf->r[MALI_COMPUTE_SR_JOB_SIZE_X];
    c->job_size[1] = rf->r[MALI_COMPUTE_SR_JOB_SIZE_Y];
    c->job_size[2] = rf->r[MALI_COMPUTE_SR_JOB_SIZE_Z];
    c->srt = R64(rf, MALI_COMPUTE_SR_SRT_0);
    c->fau = R64(rf, MALI_COMPUTE_SR_FAU_0);
    c->spd = R64(rf, MALI_COMPUTE_SR_SPD_0);
    c->tsd = R64(rf, MALI_COMPUTE_SR_TSD_0);
    c->wg = rf->r[MALI_COMPUTE_SR_WG_SIZE];
}

static void
test_compute(const mali_ctx_t *ctx)
{
    g_nchunks = 0;
    mali_arena_t a;
    mali_arena_init(&a, &rec_ops, NULL);
    mali_cs_t *cs = mali_cs_new(ctx, &a);

    mali_compute_pipeline_t pipe;
    memset(&pipe, 0, sizeof(pipe));
    upload_shader(&pipe.cs, &a, 3, 0x400000000ull);
    mali_stage_binds_t binds;
    fill_binds(&binds, &a, NULL);
    binds.tg_mem_len[0] = 100;

    mali_dispatch_t d = {.pipe = &pipe, .grid = {10, 4, 2}, .tg_size = {8, 8, 1}, .threads_per_grid = {80, 32, 2},
                         .binds = &binds, .desc_arena = &a};
    CHECK(mali_cs_dispatch(cs, ctx, &d), "dispatch");
    mali_cs_encoder_barrier(cs);

    uint64_t root;
    uint32_t size;
    CHECK(mali_cs_end(cs, &root, &size), "cs_end");
    CHECK((root & 63) == 0 && (size & 7) == 0 && size > 0, "stream alignment root=%llx size=%u", (unsigned long long)root, size);

    /* find the stream in the first arena chunk */
    const uint64_t *ins = host_of(root);
    CHECK(ins != NULL, "stream host pointer");
    struct regfile rf;
    memset(&rf, 0, sizeof(rf));
    int counts[256] = {0};
    struct run_ctx rc;
    memset(&rc, 0, sizeof(rc));
    decode_stream(ins, size / 8, &rf, counts, on_run_compute, &rc);
    CHECK(counts[MALI_CS_OPCODE_REQ_RESOURCE] == 1, "REQ_RESOURCE x1 (got %d)", counts[MALI_CS_OPCODE_REQ_RESOURCE]);
    CHECK(counts[MALI_CS_OPCODE_HEAP_SET] == 0, "a compute-only stream sets no heap");
    CHECK(counts[MALI_CS_OPCODE_FLUSH_CACHE2] == 1, "one barrier flush");
    CHECK(rc.runs == 1, "one RUN_COMPUTE (got %u)", rc.runs);
    CHECK(rc.job_size[0] == 10 && rc.job_size[1] == 4 && rc.job_size[2] == 2, "job size %u %u %u", rc.job_size[0], rc.job_size[1], rc.job_size[2]);
    CHECK(rc.spd == pipe.cs.spd_gpu[0], "SPD_0");
    CHECK((rc.srt & 63) == 8, "SRT count field 8");
    CHECK((rc.fau >> 56) == 3, "FAU slots 3");
    pan_cast_and_unpack(&rc.wg, COMPUTE_SIZE_WORKGROUP, wg);
    CHECK(wg.workgroup_size_x == 8 && wg.workgroup_size_y == 8 && wg.workgroup_size_z == 1, "WG_SIZE");

    /* the SRT array: 5 tables, entries 1 (buffers) and 2 (textures-null) and 3 (samplers) populated */
    mali_cs_free(cs);
}

/* ---- render pass ---- */
struct pass_ctx { unsigned idvs, frag; uint64_t tiler, scissor, fbd, blend, zsd, vs_srt, fs_srt; uint32_t dcd0, dcd1, dcd2, vary, tf; uint64_t spd0, spd1, spd2; };
static void
on_run_pass(unsigned op, const struct regfile *rf, void *u)
{
    struct pass_ctx *c = u;
    if (op == MALI_CS_OPCODE_RUN_IDVS) {
        c->idvs++;
        c->tiler = R64(rf, MALI_IDVS_SR_TILER_CTX);
        c->scissor = R64(rf, MALI_IDVS_SR_SCISSOR_BOX);
        c->blend = R64(rf, MALI_IDVS_SR_BLEND_DESC);
        c->zsd = R64(rf, MALI_IDVS_SR_ZSD);
        c->vs_srt = R64(rf, MALI_IDVS_SR_VERTEX_SRT);
        c->fs_srt = R64(rf, MALI_IDVS_SR_FRAGMENT_SRT);
        c->dcd0 = rf->r[MALI_IDVS_SR_DCD0];
        c->dcd1 = rf->r[MALI_IDVS_SR_DCD1];
        c->dcd2 = rf->r[MALI_IDVS_SR_DCD2];
        c->vary = rf->r[MALI_IDVS_SR_VARY_SIZE];
        c->tf = rf->r[MALI_IDVS_SR_TILER_FLAGS];
        c->spd0 = R64(rf, MALI_IDVS_SR_SPD_0);
        c->spd1 = R64(rf, MALI_IDVS_SR_SPD_1);
        c->spd2 = R64(rf, MALI_IDVS_SR_SPD_2);
    } else if (op == MALI_CS_OPCODE_RUN_FRAGMENT) {
        c->frag++;
        c->fbd = R64(rf, MALI_FRAGMENT_SR_FBD_POINTER);
    }
}

static void
test_render(const mali_ctx_t *ctx)
{
    g_nchunks = 0;
    mali_arena_t a;
    mali_arena_init(&a, &rec_ops, NULL);

    /* a 256x128 BGRA8 target */
    mali_texture_params_t tp = {.mtl_pixel_format = 80, .mtl_texture_type = MALI_TEX_2D, .width = 256, .height = 128,
                                .depth = 1, .array_length = 1, .mip_levels = 1, .sample_count = 1};
    uint64_t tsz;
    mali_texture_t *target = mali_texture_new(ctx, &tp, &tsz);
    CHECK(target != NULL, "target");
    mali_texture_set_base(target, 0x700000000ull);
    mali_texture_t *src = mali_texture_new(ctx, &tp, &tsz);
    mali_texture_set_base(src, 0x710000000ull);

    mali_gpu_ptr_t qstate = mali_arena_alloc(&a, MALI_QSTATE_SIZE, 4096);
    mali_cs_t *cs = mali_cs_new(ctx, &a);

    mali_pass_desc_t pd;
    memset(&pd, 0, sizeof(pd));
    pd.width = 256;
    pd.height = 128;
    pd.sample_count = 1;
    pd.layers = 1;
    pd.color_count = 1;
    pd.color[0] = (mali_attachment_t){.tex = target, .load = MALI_LOAD_CLEAR, .store = MALI_STORE_STORE,
                                      .clear_color = {1.0f, 0.5f, 0.0f, 1.0f}};
    pd.heap_desc_gpu = qstate.gpu;
    pd.desc_arena = &a;
    mali_pass_t *pass = mali_pass_begin(cs, ctx, &pd);
    CHECK(pass != NULL, "pass begin");
    if (!pass)
        return;

    mali_gfx_pipeline_t pipe;
    memset(&pipe, 0, sizeof(pipe));
    upload_shader(&pipe.vs, &a, 1, 0x400001000ull);
    upload_shader(&pipe.fs, &a, 2, 0x400002000ull);
    pipe.vs.vary_size_B = 32;
    pipe.rt_count = 1;
    pipe.rt[0] = (mali_rt_state_t){.mtl_format = 80, .blend_enable = true, .src_rgb = 1 /* One */, .dst_rgb = 5 /* 1-SrcA */,
                                   .op_rgb = 0, .src_alpha = 1, .dst_alpha = 5, .op_alpha = 0, .write_mask = 0xf};
    pipe.sample_count = 1;
    pipe.rasterization_enabled = true;
    CHECK(!mali_blend_rt_needs_shader(&pipe.rt[0], (float[4]){0, 0, 0, 0}), "premultiplied source-over is fixed function");

    mali_stage_binds_t vsb, fsb;
    fill_binds(&vsb, &a, NULL);
    fill_binds(&fsb, &a, src);
    mali_draw_state_t ds;
    memset(&ds, 0, sizeof(ds));
    ds.pipe = &pipe;
    ds.vs_binds = &vsb;
    ds.fs_binds = &fsb;
    ds.viewport = (typeof(ds.viewport)){0, 0, 256, 128, 0, 1};
    ds.viewport_set = true;
    ds.prim = 4;                       /* triangle strip */
    ds.count = 4;
    ds.instance_count = 1;
    ds.cull_mode = 0;
    ds.desc_arena = &a;
    CHECK(mali_pass_draw(pass, &ds), "draw");
    ds.indexed = true;
    ds.index_gpu = 0x100020000ull;
    ds.index_buffer_size = 64;
    ds.index_type = 0;
    ds.prim = 3;
    ds.count = 6;
    CHECK(mali_pass_draw(pass, &ds), "indexed draw");
    CHECK(mali_pass_end(pass), "pass end");
    mali_cs_encoder_barrier(cs);

    uint64_t root;
    uint32_t size;
    CHECK(mali_cs_end(cs, &root, &size), "cs end");
    const uint64_t *ins = host_of(root);
    CHECK(ins != NULL, "stream host pointer");
    struct regfile rf;
    memset(&rf, 0, sizeof(rf));
    int counts[256] = {0};
    struct pass_ctx pc;
    memset(&pc, 0, sizeof(pc));
    decode_stream(ins, size / 8, &rf, counts, on_run_pass, &pc);
    CHECK(pc.idvs == 2 && pc.frag == 1, "idvs %u frag %u", pc.idvs, pc.frag);
    CHECK(counts[MALI_CS_OPCODE_HEAP_SET] == 1 && counts[MALI_CS_OPCODE_REQ_RESOURCE] == 1, "prologue + heap once");
    CHECK(counts[MALI_CS_OPCODE_FINISH_TILING] == 1 && counts[MALI_CS_OPCODE_FINISH_FRAGMENT] == 1, "finish ops");
    CHECK(counts[MALI_CS_OPCODE_HEAP_OPERATION] == 2, "vt_start + vt_end (got %d)", counts[MALI_CS_OPCODE_HEAP_OPERATION]);
    CHECK(pc.spd0 == pipe.vs.spd_gpu[1], "triangle strip uses the no-point-size position SPD");
    CHECK(pc.vary == 32, "vary size");

    /* tiler context */
    struct mali_tiler_context_packed *tc = host_of(pc.tiler);
    CHECK(tc != NULL, "tiler ctx");
    if (tc) {
        pan_cast_and_unpack(tc, TILER_CONTEXT, t);
        CHECK(t.fb_width == 256 && t.fb_height == 128, "tiler fb %ux%u", t.fb_width, t.fb_height);
        CHECK(t.heap == qstate.gpu + MALI_QSTATE_HEAP_DESC_OFF, "tiler heap points at the queue-state descriptor");
        CHECK(t.geometry_buffer == qstate.gpu + MALI_QSTATE_GEOM_OFF, "geometry buffer");
        CHECK(t.hierarchy_mask != 0, "hierarchy mask %x", t.hierarchy_mask);
        CHECK(t.first_provoking_vertex, "provoking vertex first");
    }
    /* framebuffer descriptor */
    struct mali_framebuffer_packed *fbd = host_of(pc.fbd & ~63ull);
    CHECK(fbd != NULL, "fbd host");
    if (fbd) {
        pan_cast_and_unpack(fbd, FRAMEBUFFER_PARAMETERS, f);
        CHECK(f.width == 256 && f.height == 128, "fbd size %ux%u", f.width, f.height);
        CHECK(f.bound_max_x == 255 && f.bound_max_y == 127, "fbd bounds");
        CHECK(f.render_target_count == 1, "fbd rt count");
        CHECK(f.tiler == pc.tiler, "fbd tiler pointer");
        const struct mali_render_target_packed *rt = (const void *)((const uint8_t *)fbd + MALI_FRAMEBUFFER_LENGTH);
        pan_cast_and_unpack(rt, RGB_RENDER_TARGET, r);
        CHECK(r.write_enable, "RT0 write enable");
        CHECK(r.clean_tile_write_enable, "clear writes clean tiles");
        CHECK(r.writeback_buffer.base == 0x700000000ull, "RT0 writeback base %llx", (unsigned long long)r.writeback_buffer.base);
        CHECK(r.writeback_buffer.row_stride == 1024, "RT0 row stride %u", r.writeback_buffer.row_stride);
    }
    /* blend descriptor: fixed function, rt0 */
    struct mali_blend_packed *bd = host_of(pc.blend & ~63ull);
    CHECK(bd != NULL && (pc.blend & 63) == 1, "blend count 1");
    if (bd) {
        pan_cast_and_unpack(bd, BLEND, b);
        CHECK(b.enable && b.internal.mode == MALI_BLEND_MODE_FIXED_FUNCTION, "blend mode %d", (int)b.internal.mode);
    }
    /* scissor register is the viewport rect */
    uint64_t sc = pc.scissor;
    struct mali_scissor_packed sp;
    memcpy(&sp, &sc, 8);
    pan_cast_and_unpack(&sp, SCISSOR, su);
    CHECK(su.scissor_minimum_x == 0 && su.scissor_maximum_x == 255 && su.scissor_maximum_y == 127, "scissor %u %u %u %u",
          su.scissor_minimum_x, su.scissor_minimum_y, su.scissor_maximum_x, su.scissor_maximum_y);
    /* DCD: front_face_ccw inverted (winding 0 = clockwise) */
    pan_cast_and_unpack(&pc.dcd0, DCD_FLAGS_0, d0);
    CHECK(d0.front_face_ccw && !d0.cull_front_face && !d0.cull_back_face, "dcd0");
    pan_cast_and_unpack(&pc.dcd1, DCD_FLAGS_1, d1);
    CHECK(d1.render_target_mask == 1, "rt mask %x", d1.render_target_mask);

    mali_cs_free(cs);
    mali_texture_free(target);
    mali_texture_free(src);
}

int
main(void)
{
    mali_dev_props_t props;
    mali_dev_props_husky_default(&props);
    mali_ctx_t ctx;
    int rc = mali_ctx_init(&ctx, &props);
    CHECK(rc == 0, "ctx init rc=%d", rc);
    if (rc)
        return 1;
    CHECK(ctx.core_id_range == 25, "core id range %u", ctx.core_id_range);
    CHECK(ctx.tiler_max_levels == 8, "tiler levels %u", ctx.tiler_max_levels);
    CHECK(ctx.tile_rt_budget_B >= 1024, "tile budget %u", ctx.tile_rt_budget_B);

    test_formats();
    test_texture(&ctx);
    test_sampler();
    test_compute(&ctx);
    test_render(&ctx);

    printf(g_fail ? "FAILED: %d check(s)\n" : "ALL PASSED\n", g_fail);
    return g_fail != 0;
}
