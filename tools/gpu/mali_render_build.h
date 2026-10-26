/*
 * mali_render_build.h - build Mali-G715 (Valhall arch 11) render jobs:
 *   1. Clear job: fragment-only pass clearing 256x256 RGBA8 framebuffer to Colour A.
 *   2. Triangle job: IDVS pass-through vertex shader + constant Colour B fragment shader
 *      + tiler heap setup + fragment pass yielding Colour B triangle over Colour A.
 *
 * C++17, header-only. Uses Mesa genxml packers and cs_builder as a build-time library.
 */
#ifndef MALI_RENDER_BUILD_H
#define MALI_RENDER_BUILD_H

#ifndef PAN_ARCH
#define PAN_ARCH 11
#endif

#if defined(__has_include)
#  if __has_include("genxml/cs_builder.h")
#    include "genxml/cs_builder.h"
#  else
#    include "cs_builder.h"
#  endif
#else
#  include "genxml/cs_builder.h"
#endif

#include <stdint.h>
#include <string.h>
#include <vector>

#include "tri_vs_kernel.h"
#include "tri_fs_kernel.h"

namespace mr {

static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* Fixed GPU Virtual Address Map (PACKAGE.md) */
constexpr uint64_t GPU_EXEC_VA = 0x07000000ull; /* 64 KiB: executable, read-only */
constexpr uint64_t GPU_DESC_VA = 0x07010000ull; /* Descriptors, vertex buffer, RW */
constexpr uint64_t GPU_GEOM_VA = 0x07020000ull; /* 64 KiB: geometry buffer */
constexpr uint64_t GPU_HEAP_VA = 0x07100000ull; /* 1 MiB: tiler heap chunk */
constexpr uint64_t GPU_FB_VA   = 0x07200000ull; /* 256 KiB: framebuffer linear RGBA8 */

constexpr uint32_t FB_WIDTH          = 256;
constexpr uint32_t FB_HEIGHT         = 256;
constexpr uint32_t FB_BYTES_PER_PIXEL= 4;
constexpr uint32_t FB_STRIDE         = FB_WIDTH * FB_BYTES_PER_PIXEL; // 1024
constexpr uint32_t FB_BYTES          = FB_HEIGHT * FB_STRIDE;         // 262144 (256 KiB)

constexpr uint32_t TILER_HEAP_SIZE   = 0x100000; // 1 MiB
constexpr uint32_t GEOM_BUF_SIZE     = 0x10000;  // 64 KiB

/* Colour A: Deep Blue / Cornflower Blue
 * RGBA: (32, 64, 128, 255) -> hex LE: 0xFF804020 */
constexpr uint8_t  COLOR_A_R = 0x20;
constexpr uint8_t  COLOR_A_G = 0x40;
constexpr uint8_t  COLOR_A_B = 0x80;
constexpr uint8_t  COLOR_A_A = 0xFF;
constexpr uint32_t COLOR_A_PACKED = 0xFF804020u;

/* Colour B: Coral / Gold / Orange
 * RGBA: (255, 128, 64, 255) -> hex LE: 0xFF4080FF */
constexpr uint8_t  COLOR_B_R = 0xFF;
constexpr uint8_t  COLOR_B_G = 0x80;
constexpr uint8_t  COLOR_B_B = 0x40;
constexpr uint8_t  COLOR_B_A = 0xFF;
constexpr uint32_t COLOR_B_PACKED = 0xFF4080FFu;

/* Hardware sample positions: 64 entries of 4 bytes (8:8 fixed point).
 * Single-sampled (pattern 0): center is (0.5, 0.5) -> (128, 128). */
struct SamplePositions {
    struct { uint16_t x, y; } positions[32];
    struct { uint16_t x, y; } origin;
    struct { uint16_t x, y; } padding[31];
};

static inline void init_sample_positions(SamplePositions *sp) {
    memset(sp, 0, sizeof(*sp));
    sp->positions[0].x = 128;
    sp->positions[0].y = 128;
    sp->origin.x = 128;
    sp->origin.y = 128;
}

/* Common builder configuration for user CS streams */
static inline cs_builder_conf render_cs_conf() {
    cs_builder_conf conf = {};
    conf.nr_registers = 96;       // 96 work registers on Mali-G715 CS0
    conf.nr_kernel_registers = 4; // r92..r95 kernel wrapper
    conf.compute_ep_limit = 0;
    conf.ls_sb_slot = 0;          // slot 0 for Load/Store operations
    return conf;
}

/* =========================================================================
 * Job 1: Clear Job
 * ========================================================================= */
struct ClearJobBuilt {
    std::vector<uint8_t>  exec_page; // 4 KiB clean page at GPU_EXEC_VA
    std::vector<uint8_t>  rw_page;   // descriptors at GPU_DESC_VA
    std::vector<uint64_t> cs_words;  // ring buffer command stream
    uint32_t stream_size_bytes;
};

static inline bool emit_clear_job(ClearJobBuilt *out) {
    out->exec_page.assign(4096, 0);
    out->rw_page.assign(4096, 0);

    uint8_t *rw_cpu = out->rw_page.data();
    uint64_t rw_va  = GPU_DESC_VA;

    // Layout in RW page:
    // 0x000: Sample locations (256 B, 64-byte aligned)
    // 0x100: FBD (128 B, 64-byte aligned)
    // 0x180: RTD 0 (64 B, 64-byte aligned)
    uint32_t sp_off  = 0x000;
    uint32_t fbd_off = 0x100;
    uint32_t rtd_off = 0x180;

    uint64_t sp_va  = rw_va + sp_off;
    uint64_t fbd_va = rw_va + fbd_off;

    SamplePositions sp;
    init_sample_positions(&sp);
    memcpy(rw_cpu + sp_off, &sp, sizeof(sp));

    // Pack Framebuffer Parameters
    struct mali_framebuffer_packed fbd = {};
    struct MALI_FRAMEBUFFER_PARAMETERS fbd_params = {};
    fbd_params.width = FB_WIDTH;
    fbd_params.height = FB_HEIGHT;
    fbd_params.bound_min_x = 0;
    fbd_params.bound_min_y = 0;
    fbd_params.bound_max_x = FB_WIDTH - 1;
    fbd_params.bound_max_y = FB_HEIGHT - 1;
    fbd_params.sample_locations = sp_va;
    fbd_params.sample_count = 1;
    fbd_params.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
    fbd_params.effective_tile_size = 16 * 16;
    fbd_params.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
    fbd_params.render_target_count = 1;
    fbd_params.color_buffer_allocation = 1024;
    fbd_params.first_provoking_vertex = true;
    MALI_FRAMEBUFFER_SECTION_PARAMETERS_pack(pan_section_ptr(&fbd, FRAMEBUFFER, PARAMETERS), &fbd_params);
    memcpy(rw_cpu + fbd_off, &fbd, sizeof(fbd));

    // Pack Render Target 0
    struct mali_render_target_packed rtd = {};
    pan_cast_and_pack(&rtd, RGB_RENDER_TARGET, cfg) {
        cfg.internal_buffer_offset = 0;
        cfg.dithering_enable = true;
        cfg.write_enable = true;
        cfg.clean_tile_write_enable = true;
        cfg.internal_format = MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
        cfg.writeback_format = MALI_COLOR_FORMAT_R8G8B8A8;
        cfg.writeback_block_format = MALI_BLOCK_FORMAT_LINEAR;
        cfg.swizzle = 0x688; // identity RGBA, 3 bits per channel (Mesa pan_util.c:15-24, pan_desc.c:807-809)
        cfg.writeback_buffer.base = GPU_FB_VA;
        cfg.writeback_buffer.row_stride = FB_STRIDE;
        cfg.writeback_buffer.surface_stride = FB_BYTES;
        cfg.clear.color_0 = COLOR_A_PACKED;
        cfg.clear.color_1 = COLOR_A_PACKED;
        cfg.clear.color_2 = COLOR_A_PACKED;
        cfg.clear.color_3 = COLOR_A_PACKED;
    }
    memcpy(rw_cpu + rtd_off, &rtd, sizeof(rtd));

    // Emit CS stream
    std::vector<uint64_t> cs_buf(256, 0);
    cs_builder_conf conf = render_cs_conf();
    cs_buffer cb_buf = {};
    cb_buf.cpu = cs_buf.data();
    cb_buf.gpu = 0;
    cb_buf.capacity = cs_buf.size();

    cs_builder b;
    cs_builder_init(&b, &conf, cb_buf);

    // Request Fragment resource
    cs_req_res(&b, CS_FRAG_RES);

    // Scoreboard configuration: endpoint on slot 1, LS on slot 0
    cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_ENDPOINT, 1);
    cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_OTHER, 0);

    // FRAGMENT staging registers
    // r40:41 = FBD_POINTER (FBD VA | tag 1)
    cs_move64_to(&b, cs_sr_reg64(&b, FRAGMENT, FBD_POINTER), fbd_va | 1);
    // r42 = BBOX_MIN
    cs_move32_to(&b, cs_sr_reg32(&b, FRAGMENT, BBOX_MIN), 0);
    // r43 = BBOX_MAX: (255) | (255 << 16)
    cs_move32_to(&b, cs_sr_reg32(&b, FRAGMENT, BBOX_MAX), (FB_WIDTH - 1) | ((FB_HEIGHT - 1) << 16));

    // RUN_FRAGMENT: no TEM, Z-order tile rendering
    cs_run_fragment(&b, false, MALI_TILE_RENDER_ORDER_Z_ORDER);

    // Wait for fragment pass on endpoint (slot 1)
    cs_wait_slot(&b, 1);

    // Cache clean and invalidate to ensure framebuffer writes are visible to memory
    cs_move32_to(&b, cs_reg32(&b, 0), 0);
    cs_flush_caches(&b, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                    MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                    MALI_CS_OTHER_FLUSH_MODE_INVALIDATE,
                    cs_reg32(&b, 0), cs_defer(0, 0));
    cs_wait_slot(&b, 0);

    if (!cs_is_valid(&b))
        return false;

    cs_end(&b);
    uint32_t sz = cs_root_chunk_size(&b);
    out->stream_size_bytes = sz;
    out->cs_words.assign(cs_buf.begin(), cs_buf.begin() + (sz / 8));
    return true;
}

/* =========================================================================
 * Job 2: Triangle Job
 * ========================================================================= */
struct TriangleJobBuilt {
    std::vector<uint8_t>  exec_page; // shader code + SPDs + TSD + FAU at GPU_EXEC_VA
    std::vector<uint8_t>  rw_page;   // descriptors + vertex buffer at GPU_DESC_VA
    std::vector<uint64_t> cs_words;  // ring buffer command stream
    uint32_t stream_size_bytes;
};

static inline bool emit_triangle_job(TriangleJobBuilt *out) {
    out->exec_page.assign(4096, 0);
    out->rw_page.assign(4096, 0);

    uint8_t *exec_cpu = out->exec_page.data();
    uint64_t exec_va  = GPU_EXEC_VA;
    uint8_t *rw_cpu   = out->rw_page.data();
    uint64_t rw_va    = GPU_DESC_VA;

    // --- Executable Page Layout ---
    // 0x000: FAU (64 B): slot 0 = vertex buffer pointer
    // 0x080: VS code (128-byte aligned, 40 B)
    // 0x100: FS code (128-byte aligned, 40 B)
    // 0x180: TSD (64 B, 64-byte aligned)
    // 0x200: VS SPD 0 (Position variant, 32 B, 32-byte aligned)
    // 0x220: VS SPD 1 (Points / secondary variant, 32 B)
    // 0x240: FS SPD (32 B, 32-byte aligned)
    uint32_t fau_off    = 0x000;
    uint32_t vs_off     = 0x080;
    uint32_t fs_off     = 0x100;
    uint32_t tsd_off    = 0x180;
    uint32_t vs_spd_off = 0x200;
    uint32_t fs_spd_off = 0x240;

    uint64_t fau_va    = exec_va + fau_off;
    uint64_t vs_va     = exec_va + vs_off;
    uint64_t fs_va     = exec_va + fs_off;
    uint64_t tsd_va    = exec_va + tsd_off;
    uint64_t vs_spd_va = exec_va + vs_spd_off;
    uint64_t fs_spd_va = exec_va + fs_spd_off;

    // --- RW Page Layout ---
    // 0x000: Sample locations (256 B)
    // 0x100: Vertex buffer (48 B: 3 vertices * 16 B, 64-byte aligned)
    // 0x200: TILER_HEAP descriptor (32 B, 32-byte aligned)
    // 0x240: Heap context (32 B, 32-byte aligned)
    // 0x300: TILER_CONTEXT (128 B, 64-byte aligned)
    // 0x400: BLEND descriptor (16 B, 64-byte aligned)
    // 0x440: DEPTH_STENCIL descriptor (32 B, 32-byte aligned)
    // 0x500: FBD (128 B, 64-byte aligned)
    // 0x580: RTD 0 (64 B, 64-byte aligned)
    uint32_t sp_off        = 0x000;
    uint32_t vb_off        = 0x100;
    uint32_t heap_desc_off = 0x200;
    uint32_t heap_ctx_off  = 0x240;
    uint32_t tiler_ctx_off = 0x300;
    uint32_t blend_off     = 0x400;
    uint32_t zsd_off       = 0x440;
    uint32_t fbd_off       = 0x500;
    uint32_t rtd_off       = 0x580;

    uint64_t sp_va        = rw_va + sp_off;
    uint64_t vb_va        = rw_va + vb_off;
    uint64_t heap_desc_va = rw_va + heap_desc_off;
    uint64_t heap_ctx_va  = rw_va + heap_ctx_off;
    uint64_t tiler_ctx_va = rw_va + tiler_ctx_off;
    uint64_t blend_va     = rw_va + blend_off;
    uint64_t zsd_va       = rw_va + zsd_off;
    uint64_t fbd_va       = rw_va + fbd_off;

    // 1. Fill FAU: slot 0 holds 64-bit vertex buffer pointer
    uint64_t *fau = (uint64_t *)(exec_cpu + fau_off);
    fau[0] = vb_va;

    // 2. Copy Shader Binaries
    memcpy(exec_cpu + vs_off, tri_vs_code, sizeof(tri_vs_code));
    memcpy(exec_cpu + fs_off, tri_fs_code, sizeof(tri_fs_code));

    // 3. Pack TSD (Local Storage descriptor)
    struct mali_local_storage_packed tsd = {};
    pan_pack(&tsd, LOCAL_STORAGE, cfg) {
        cfg.tls_size = 0;
        cfg.tls_base_pointer = 0;
        cfg.tls_address_mode = MALI_ADDRESS_MODE_PACKED;
        cfg.wls_instances = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM;
    }
    memcpy(exec_cpu + tsd_off, &tsd, sizeof(tsd));

    // 4. Pack VS SPD (Shader Program descriptor)
    struct mali_shader_program_packed vs_spd = {};
    pan_pack(&vs_spd, SHADER_PROGRAM, cfg) {
        cfg.stage = MALI_SHADER_STAGE_VERTEX;
        cfg.vertex_warp_limit = MALI_WARP_LIMIT_HALF; // position shaders before v12 (panvk_vX_shader.c:1299,1315)
        cfg.preload.r48_r63 = (uint32_t)(TRI_VS_PRELOAD_MASK >> 48);
        cfg.register_allocation = MALI_SHADER_REGISTER_ALLOCATION_64_PER_THREAD;
        cfg.flush_to_zero_mode = MALI_FLUSH_TO_ZERO_MODE_PRESERVE_SUBNORMALS;
        cfg.binary = vs_va;
    }
    memcpy(exec_cpu + vs_spd_off, &vs_spd, sizeof(vs_spd));
    // Points / secondary variant (same binary)
    memcpy(exec_cpu + vs_spd_off + 32, &vs_spd, sizeof(vs_spd));

    // 5. Pack FS SPD
    struct mali_shader_program_packed fs_spd = {};
    pan_pack(&fs_spd, SHADER_PROGRAM, cfg) {
        cfg.stage = MALI_SHADER_STAGE_FRAGMENT;
        cfg.fragment_coverage_bitmask_type = MALI_COVERAGE_BITMASK_TYPE_GL; // panvk_vX_shader.c:1243
        cfg.preload.r48_r63 = (uint32_t)(TRI_FS_PRELOAD_MASK >> 48);
        cfg.register_allocation = MALI_SHADER_REGISTER_ALLOCATION_64_PER_THREAD;
        cfg.flush_to_zero_mode = MALI_FLUSH_TO_ZERO_MODE_PRESERVE_SUBNORMALS;
        cfg.binary = fs_va;
    }
    memcpy(exec_cpu + fs_spd_off, &fs_spd, sizeof(fs_spd));

    // 6. Sample Positions
    SamplePositions sp;
    init_sample_positions(&sp);
    memcpy(rw_cpu + sp_off, &sp, sizeof(sp));

    // 7. Vertex Buffer (Screenspace XYZ and 1/W for IDVS pass-through)
    // Vertices:
    // V0: (128.0, 32.0,  0.5, 1.0) - top center
    // V1: (32.0,  224.0, 0.5, 1.0) - bottom left
    // V2: (224.0, 224.0, 0.5, 1.0) - bottom right
    float vertices[3][4] = {
        { 128.0f,  32.0f, 0.5f, 1.0f },
        {  32.0f, 224.0f, 0.5f, 1.0f },
        { 224.0f, 224.0f, 0.5f, 1.0f }
    };
    memcpy(rw_cpu + vb_off, vertices, sizeof(vertices));

    // 8. TILER_HEAP descriptor
    struct mali_tiler_heap_packed heap_desc = {};
    pan_pack(&heap_desc, TILER_HEAP, cfg) {
        cfg.size = TILER_HEAP_SIZE;
        cfg.base = GPU_HEAP_VA;
        cfg.bottom = GPU_HEAP_VA + 64;
        cfg.top = GPU_HEAP_VA + TILER_HEAP_SIZE;
    }
    memcpy(rw_cpu + heap_desc_off, &heap_desc, sizeof(heap_desc));

    // 9. Heap context: zeroed
    memset(rw_cpu + heap_ctx_off, 0, 32);

    // 10. TILER_CONTEXT
    struct mali_tiler_context_packed tiler_ctx = {};
    pan_pack(&tiler_ctx, TILER_CONTEXT, cfg) {
        cfg.hierarchy_mask = 0xFF;
        cfg.fb_width = FB_WIDTH;
        cfg.fb_height = FB_HEIGHT;
        cfg.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
        cfg.first_provoking_vertex = true;
        cfg.layer_count = 1;
        cfg.layer_offset = 0;
        cfg.heap = heap_desc_va;
        cfg.geometry_buffer = GPU_GEOM_VA;
        cfg.geometry_buffer_size = GEOM_BUF_SIZE;
        cfg.completed_top = 0;
        cfg.completed_bottom = 0;
    }
    memcpy(rw_cpu + tiler_ctx_off, &tiler_ctx, sizeof(tiler_ctx));

    // 11. BLEND descriptor
    struct mali_blend_packed blend_desc = {};
    struct MALI_BLEND blend = {};
    blend.enable = true;
    blend.round_to_fb_precision = true;
    blend.load_destination = false;
    blend.internal.mode = MALI_BLEND_MODE_OPAQUE;
    blend.equation.color_mask = 0xF;
    blend.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
    blend.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
    blend.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
    blend.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
    blend.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
    blend.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
    blend.internal.fixed_function.num_comps = 4;
    blend.internal.fixed_function.conversion.memory_format = (237 << 12); // RGBA8_TB
    MALI_BLEND_pack(&blend_desc, &blend);
    memcpy(rw_cpu + blend_off, &blend_desc, sizeof(blend_desc));

    // 12. DEPTH_STENCIL descriptor
    struct mali_depth_stencil_packed zsd = {};
    pan_pack(&zsd, DEPTH_STENCIL, cfg) {
        cfg.depth_function = MALI_FUNC_ALWAYS;
        cfg.depth_write_enable = false;
        cfg.stencil_test_enable = false;
        cfg.depth_cull_enable = false;
        cfg.depth_clamp_mode = MALI_DEPTH_CLAMP_MODE_BOUNDS;
        cfg.separated_dependency_tracking = true; // arch 11
        cfg.depth_source = MALI_DEPTH_SOURCE_FIXED_FUNCTION;
    }
    memcpy(rw_cpu + zsd_off, &zsd, sizeof(zsd));

    // 13. FRAMEBUFFER descriptor
    struct mali_framebuffer_packed fbd = {};
    struct MALI_FRAMEBUFFER_PARAMETERS fbd_params = {};
    fbd_params.width = FB_WIDTH;
    fbd_params.height = FB_HEIGHT;
    fbd_params.bound_min_x = 0;
    fbd_params.bound_min_y = 0;
    fbd_params.bound_max_x = FB_WIDTH - 1;
    fbd_params.bound_max_y = FB_HEIGHT - 1;
    fbd_params.sample_locations = sp_va;
    fbd_params.sample_count = 1;
    fbd_params.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
    fbd_params.effective_tile_size = 16 * 16;
    fbd_params.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
    fbd_params.render_target_count = 1;
    fbd_params.color_buffer_allocation = 1024;
    fbd_params.first_provoking_vertex = true;
    fbd_params.tiler = tiler_ctx_va;
    MALI_FRAMEBUFFER_SECTION_PARAMETERS_pack(pan_section_ptr(&fbd, FRAMEBUFFER, PARAMETERS), &fbd_params);
    memcpy(rw_cpu + fbd_off, &fbd, sizeof(fbd));

    // 14. Render Target 0
    struct mali_render_target_packed rtd = {};
    pan_cast_and_pack(&rtd, RGB_RENDER_TARGET, cfg) {
        cfg.internal_buffer_offset = 0;
        cfg.dithering_enable = true;
        cfg.write_enable = true;
        cfg.clean_tile_write_enable = true;
        cfg.internal_format = MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
        cfg.writeback_format = MALI_COLOR_FORMAT_R8G8B8A8;
        cfg.writeback_block_format = MALI_BLOCK_FORMAT_LINEAR;
        cfg.swizzle = 0x688; // identity RGBA, 3 bits per channel (Mesa pan_util.c:15-24, pan_desc.c:807-809)
        cfg.writeback_buffer.base = GPU_FB_VA;
        cfg.writeback_buffer.row_stride = FB_STRIDE;
        cfg.writeback_buffer.surface_stride = FB_BYTES;
        cfg.clear.color_0 = COLOR_A_PACKED;
        cfg.clear.color_1 = COLOR_A_PACKED;
        cfg.clear.color_2 = COLOR_A_PACKED;
        cfg.clear.color_3 = COLOR_A_PACKED;
    }
    memcpy(rw_cpu + rtd_off, &rtd, sizeof(rtd));

    // --- Pack Draw State and Flags ---
    struct mali_dcd_flags_0_packed dcd0;
    pan_pack(&dcd0, DCD_FLAGS_0, cfg) {
        cfg.allow_forward_pixel_to_kill = true;
        cfg.allow_forward_pixel_to_be_killed = true;
        cfg.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;
        cfg.zs_update_operation = MALI_PIXEL_KILL_FORCE_EARLY;
        cfg.front_face_ccw = true;
        cfg.cull_front_face = false;
        cfg.cull_back_face = false;
        cfg.multisample_enable = false;
        cfg.scissor_to_bounding_box = true;
        cfg.cull_zero_area = true;
    }

    struct mali_dcd_flags_1_packed dcd1;
    pan_pack(&dcd1, DCD_FLAGS_1, cfg) {
        cfg.sample_mask = 0xFFFF;
        cfg.render_target_mask = 1;
    }

    struct mali_dcd_flags_2_packed dcd2;
    pan_pack(&dcd2, DCD_FLAGS_2, cfg) {
        cfg.write_mask = 1;
        cfg.read_mask = 0;
        cfg.no_shader_depth_read = true;
        cfg.no_shader_stencil_read = true;
    }

    struct mali_primitive_flags_packed pflags;
    pan_pack(&pflags, PRIMITIVE_FLAGS, cfg) {
        cfg.draw_mode = MALI_DRAW_MODE_TRIANGLES;
        cfg.index_type = MALI_INDEX_TYPE_NONE;
        cfg.low_depth_cull = true;
        cfg.high_depth_cull = true;
        cfg.position_fifo_format = MALI_FIFO_FORMAT_BASIC;
    }

    struct mali_scissor_packed scissor;
    pan_pack(&scissor, SCISSOR, cfg) {
        cfg.scissor_minimum_x = 0;
        cfg.scissor_minimum_y = 0;
        cfg.scissor_maximum_x = FB_WIDTH - 1;
        cfg.scissor_maximum_y = FB_HEIGHT - 1;
    }
    uint64_t scissor_raw = 0;
    memcpy(&scissor_raw, &scissor, sizeof(scissor_raw));

    // --- Build Command Stream ---
    std::vector<uint64_t> cs_buf(512, 0);
    cs_builder_conf conf = render_cs_conf();
    cs_buffer cb_buf = {};
    cb_buf.cpu = cs_buf.data();
    cb_buf.gpu = 0;
    cb_buf.capacity = cs_buf.size();

    cs_builder b;
    cs_builder_init(&b, &conf, cb_buf);

    // Request required resources: compute, fragment, tiler, idvs
    cs_req_res(&b, CS_COMPUTE_RES | CS_FRAG_RES | CS_TILER_RES | CS_IDVS_RES);

    // Set scoreboard mapping
    cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_ENDPOINT, 1);
    cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_OTHER, 0);

    // Set Tiler Heap Context
    cs_move64_to(&b, cs_reg64(&b, 66), heap_ctx_va);
    cs_heap_set(&b, cs_reg64(&b, 66));

    // Set Tiler Context
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, TILER_CTX), tiler_ctx_va);

    // Mark Vertex/Tiler started
    cs_vt_start(&b, cs_now());

    // Staging registers for IDVS
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, SRT_0), 0);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, SRT_2), 0);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, FAU_0), fau_va | (1ULL << 56));
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, FAU_2), 0);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, SPD_0), vs_spd_va);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, SPD_1), vs_spd_va + 32);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, SPD_2), fs_spd_va);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, TSD_0), tsd_va);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, GLOBAL_ATTRIBUTE_OFFSET), 0);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, INDEX_COUNT), 3);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, INSTANCE_COUNT), 1);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, INDEX_OFFSET), 0);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, VERTEX_OFFSET), 0);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, INSTANCE_OFFSET), 0);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, DCD2), dcd2.opaque[0]);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, INDEX_BUFFER_SIZE), 0);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, TILER_CTX), tiler_ctx_va);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, SCISSOR_BOX), scissor_raw);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, LOW_DEPTH_CLAMP), 0);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, HIGH_DEPTH_CLAMP), 0x3F800000u);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, OQ), 0);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, VARY_SIZE), 0);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, BLEND_DESC), blend_va | 1ULL);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, ZSD), zsd_va);
    cs_move64_to(&b, cs_sr_reg64(&b, IDVS, INDEX_BUFFER), 0);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, TILER_FLAGS), pflags.opaque[0]);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, DCD0), dcd0.opaque[0]);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, DCD1), dcd1.opaque[0]);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, PRIMITIVE_SIZE), 0);
    cs_move32_to(&b, cs_sr_reg32(&b, IDVS, TILER_FLAGS2), 0);

    // Launch IDVS draw: malloc_enable = true
    cs_run_idvs(&b, 0, true, cs_shader_res_sel(0, 0, 1, 0),
                cs_shader_res_sel(0, 2, 2, 0), cs_undef());

    // Finish tiling & wait for VT completion
    cs_finish_tiling(&b);
    cs_vt_end(&b, cs_defer(1 << 1, 2)); // wait endpoint (slot 1), signal slot 2
    cs_wait_slot(&b, 2);

    // FRAGMENT staging registers
    cs_move64_to(&b, cs_sr_reg64(&b, FRAGMENT, FBD_POINTER), fbd_va | 1);
    cs_move32_to(&b, cs_sr_reg32(&b, FRAGMENT, BBOX_MIN), 0);
    cs_move32_to(&b, cs_sr_reg32(&b, FRAGMENT, BBOX_MAX), (FB_WIDTH - 1) | ((FB_HEIGHT - 1) << 16));

    // RUN_FRAGMENT: Z-order
    cs_run_fragment(&b, false, MALI_TILE_RENDER_ORDER_Z_ORDER);

    // Free tiler heap chunks from completed top/bottom at offset 40 of TILER_CONTEXT
    cs_move64_to(&b, cs_reg64(&b, 66), tiler_ctx_va);
    cs_load_to(&b, cs_reg_tuple(&b, 68, 4), cs_reg64(&b, 66), BITFIELD_MASK(4), 40);
    cs_wait_slot(&b, 0); // wait LS
    cs_finish_fragment(&b, true, cs_reg64(&b, 68), cs_reg64(&b, 70), cs_defer(1 << 1, 3));
    cs_wait_slot(&b, 3); // wait fragment completion on slot 3

    // Final cache flush and wait
    cs_move32_to(&b, cs_reg32(&b, 66), 0);
    cs_flush_caches(&b, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                    MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                    MALI_CS_OTHER_FLUSH_MODE_INVALIDATE,
                    cs_reg32(&b, 66), cs_defer(0, 0));
    cs_wait_slot(&b, 0);

    if (!cs_is_valid(&b))
        return false;

    cs_end(&b);
    uint32_t sz = cs_root_chunk_size(&b);
    out->stream_size_bytes = sz;
    out->cs_words.assign(cs_buf.begin(), cs_buf.begin() + (sz / 8));
    return true;
}

} // namespace mr

#endif // MALI_RENDER_BUILD_H
