/* SPDX-License-Identifier: MIT */
/*
 * vh_lower_bindings.cpp - Lower AIR-translated NIR bindings & sysvals to VH-ABI 1
 * (spec 07 sections 4, 6.1-6.13).
 *
 * Citations:
 * - SRT handles: pan_res_handle(table, index) = (table << 24) | index
 *   (MP/compiler/pan_compiler.h:142-160).
 * - Dummy sampler slot 16 for txf/txf_ms/txs:
 *   MP/vulkan/panvk_vX_nir_lower_descriptors.c:666-672,
 *   MP/vulkan/panvk_vX_cmd_desc_state.c:892-907.
 * - Inline constant pool lowering (load_constant -> load_global_constant):
 *   MP/vulkan/panvk_vX_nir_lower_descriptors.c:1024-1052.
 * - Sysval lowering & FAU packing:
 *   MP/vulkan/panvk_vX_shader.c:66-222, 630-790.
 */
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>

#include "vh_lower_bindings.h"
#include "nir_builder.h"
#include "nir_control_flow.h"
#include "util/bitscan.h"
#include "util/u_math.h"

static nir_def *
emit_sysval_load(nir_builder *b, unsigned num_components, unsigned bit_size,
                 uint32_t sv_byte_offset)
{
    unsigned bytes = num_components * (bit_size / 8);
    unsigned align_mul = (sv_byte_offset % 8 == 0 && bytes >= 8) ? 8 : 4;
    return nir_load_push_constant(b, num_components, bit_size,
                                  nir_imm_int(b, (int)sv_byte_offset),
                                  .base = VH_SYSVAL_PUSH_BASE,
                                  .range = bytes,
                                  .align_mul = align_mul,
                                  .align_offset = 0);
}

uint32_t
air2vh_name_hash24(const char *name)
{
    if (!name || !*name)
        return 0;
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        h = (h ^ *p) * 16777619u;
    h &= 0xffffffu;
    return h ? h : 1u;
}

void
air2vh_prepare_pre_nir(nir_shader *nir, AirFrontendResult &fe,
                       uint32_t flags, const air2vh_vs_layout *vs_layout)
{
    /* Tier-B FS varying linking by name (spec 07 6.9, MaliPipeline.m:225-233):
     * Remap FS nir_var_shader_in variable locations to match the VS's
     * VARYING_SLOT_VAR0+k assignments recorded in vs_layout->blob.
     */
    if (nir->info.stage == MESA_SHADER_FRAGMENT && vs_layout &&
        vs_layout->blob && vs_layout->blob_size >= sizeof(vh_vs_layout_blob)) {
        vh_vs_layout_blob blob;
        memcpy(&blob, vs_layout->blob, sizeof(blob));
        if (blob.magic == VH_VS_LAYOUT_MAGIC) {
            uint32_t used_generic = 0;
            for (unsigned k = 0; k < 32; k++) {
                uint32_t h = (uint32_t)blob.name_hash24[k][0] |
                             ((uint32_t)blob.name_hash24[k][1] << 8) |
                             ((uint32_t)blob.name_hash24[k][2] << 16);
                if (h != 0)
                    used_generic |= (1u << k);
            }
            nir_foreach_shader_in_variable(var, nir) {
                if (var->data.location < VARYING_SLOT_VAR0 ||
                    var->data.location > VARYING_SLOT_VAR31)
                    continue;
                int matched_slot = -1;
                uint32_t want_h = air2vh_name_hash24(var->name);
                if (want_h != 0) {
                    for (unsigned k = 0; k < 32; k++) {
                        uint32_t h = (uint32_t)blob.name_hash24[k][0] |
                                     ((uint32_t)blob.name_hash24[k][1] << 8) |
                                     ((uint32_t)blob.name_hash24[k][2] << 16);
                        if (h == want_h) {
                            matched_slot = (int)k;
                            break;
                        }
                    }
                }
                if (matched_slot >= 0) {
                    var->data.location = VARYING_SLOT_VAR0 + matched_slot;
                } else {
                    unsigned slot = 0;
                    while (slot < 31 && (used_generic & (1u << slot)))
                        slot++;
                    used_generic |= (1u << slot);
                    matched_slot = (int)slot;
                    var->data.location = VARYING_SLOT_VAR0 + slot;
                }
                if (var->name) {
                    for (auto &vy : fe.varyings) {
                        if (vy.name == var->name)
                            vy.slot = matched_slot;
                    }
                }
            }
        }
    }

    /* Non-uniform dispatchThreads guard (spec 07 3.3, 6.12):
     * Wrap the compute kernel body in:
     *   if (all(global_invocation_id < threads_per_grid)) { <body> }
     * Inserted before pan_preprocess_nir so nir_lower_compute_system_values
     * and nir_lower_alu_width lower global_invocation_id and ball_iequal3.
     */
    if (nir->info.stage == MESA_SHADER_COMPUTE &&
        (flags & AIR2VH_F_NONUNIFORM_DISPATCH)) {
        nir_function_impl *impl = nir_shader_get_entrypoint(nir);
        nir_cf_list body;
        nir_cf_list_extract(&body, &impl->body);

        nir_builder b = nir_builder_at(nir_after_cf_list(&impl->body));
        nir_def *gid = nir_load_global_invocation_id(&b, 32);
        nir_def *tpg = nir_load_ubo(
            &b, 3, 32, nir_imm_int(&b, fe.driver_table_ubo),
            nir_imm_int(&b, 384 /* ABI_TPG_TABLE_OFFSET */),
            .align_mul = 4, .align_offset = 0, .range_base = 0, .range = ~0u);
        nir_def *in_bounds = nir_ball(&b, nir_ult(&b, gid, tpg));
        nir_if *nif = nir_push_if(&b, in_bounds);
        nir_cf_reinsert(&body, nir_before_cf_list(&nif->then_list));
        nir_pop_if(&b, nif);

        nir_progress(true, impl, nir_metadata_none);
    }
}

namespace {

struct LowerBindingsCtx {
    int driver_table_ubo;
    mesa_shader_stage stage;
    uint16_t *table_count;
};

static bool
lower_binding_instr(nir_builder *b, nir_instr *instr, void *data)
{
    auto *ctx = static_cast<LowerBindingsCtx *>(data);
    b->cursor = nir_before_instr(instr);

    if (instr->type == nir_instr_type_tex) {
        nir_tex_instr *tex = nir_instr_as_tex(instr);
        /* Skip if handles were already attached. */
        if (nir_tex_instr_src_index(tex, nir_tex_src_texture_handle) >= 0)
            return false;

        uint32_t t = tex->texture_index;
        uint32_t s = tex->sampler_index;
        if (tex->op == nir_texop_txf || tex->op == nir_texop_txf_ms ||
            tex->op == nir_texop_txs || tex->op == nir_texop_lod ||
            tex->op == nir_texop_samples_identical ||
            tex->op == nir_texop_query_levels) {
            if (tex->op == nir_texop_txf || tex->op == nir_texop_txf_ms ||
                tex->op == nir_texop_txs || tex->op == nir_texop_samples_identical ||
                tex->op == nir_texop_query_levels) {
                s = AIR2VH_DUMMY_SAMPLER_INDEX;
            }
        }
        if (t >= 128u)
            throw AirError("texture index " + std::to_string(t) + " exceeds 128");
        if (s > AIR2VH_DUMMY_SAMPLER_INDEX)
            throw AirError("sampler index " + std::to_string(s) + " exceeds AIR2VH_DUMMY_SAMPLER_INDEX");

        uint32_t tex_h = AIR2VH_RES_HANDLE(AIR2VH_T_TEXTURES, t);
        uint32_t samp_h = AIR2VH_RES_HANDLE(AIR2VH_T_SAMPLERS, s);

        nir_tex_instr_add_src(tex, nir_tex_src_texture_handle,
                              nir_imm_int(b, (int)tex_h));
        nir_tex_instr_add_src(tex, nir_tex_src_sampler_handle,
                              nir_imm_int(b, (int)samp_h));
        tex->texture_index = 0;
        tex->sampler_index = 0;

        ctx->table_count[AIR2VH_T_TEXTURES] =
            std::max<uint16_t>(ctx->table_count[AIR2VH_T_TEXTURES], (uint16_t)(t + 1));
        ctx->table_count[AIR2VH_T_SAMPLERS] =
            std::max<uint16_t>(ctx->table_count[AIR2VH_T_SAMPLERS], (uint16_t)(s + 1));
        return true;
    }

    if (instr->type != nir_instr_type_intrinsic)
        return false;

    nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
    switch (intr->intrinsic) {
    case nir_intrinsic_load_ubo: {
        if (!nir_src_is_const(intr->src[0]))
            throw AirError("dynamic UBO index is not supported in VH-ABI 1");
        uint32_t ubo_idx = nir_src_as_uint(intr->src[0]);

        /* Check if already lowered to a pan_res_handle (table > 0). */
        if ((ubo_idx >> 24) != 0)
            return false;

        if ((int)ubo_idx == ctx->driver_table_ubo) {
            if (!nir_src_is_const(intr->src[1]))
                throw AirError("dynamic offset into driver_table_ubo");
            uint32_t off = nir_src_as_uint(intr->src[1]);
            uint32_t sv_off = 0;
            if (off < 256) {
                unsigned n = off / 8;
                unsigned rem = off % 8;
                if (n >= 31u)
                    throw AirError("device buffer index " + std::to_string(n) + " out of range");
                sv_off = (ctx->stage == MESA_SHADER_COMPUTE
                              ? (uint32_t)offsetof(air2vh_sysvals_cs, buffer_va[0])
                              : (uint32_t)offsetof(air2vh_sysvals_gfx, buffer_va[0])) +
                         n * 8u + rem;
            } else if (off >= 256 && off < 384) {
                unsigned n = (off - 256) / 4;
                unsigned rem = (off - 256) % 4;
                if (n >= 31u)
                    throw AirError("threadgroup buffer index " + std::to_string(n) + " out of range");
                sv_off = (uint32_t)offsetof(air2vh_sysvals_cs, tg_offset[0]) + n * 4u + rem;
            } else if (off >= 384 && off < 396) {
                unsigned rem = off - 384;
                sv_off = (uint32_t)offsetof(air2vh_sysvals_cs, threads_per_grid) + rem;
            } else {
                throw AirError("unexpected driver_table_ubo byte offset " + std::to_string(off));
            }
            nir_def *val = emit_sysval_load(b, intr->def.num_components,
                                            intr->def.bit_size, sv_off);
            nir_def_replace(&intr->def, val);
            return true;
        }

        if (ubo_idx >= 31u)
            throw AirError("UBO buffer index " + std::to_string(ubo_idx) + " exceeds 31");
        uint32_t handle = AIR2VH_RES_HANDLE(AIR2VH_T_BUFFERS, ubo_idx);
        nir_src_rewrite(&intr->src[0], nir_imm_int(b, (int)handle));
        ctx->table_count[AIR2VH_T_BUFFERS] =
            std::max<uint16_t>(ctx->table_count[AIR2VH_T_BUFFERS], (uint16_t)(ubo_idx + 1));
        return true;
    }

    case nir_intrinsic_image_store:
    case nir_intrinsic_image_load:
    case nir_intrinsic_image_size:
    case nir_intrinsic_image_samples:
    case nir_intrinsic_image_texel_address: {
        if (!nir_src_is_const(intr->src[0]))
            throw AirError("dynamic image index is not supported in VH-ABI 1");
        uint32_t img_idx = nir_src_as_uint(intr->src[0]);
        if ((img_idx >> 24) != 0)
            return false;
        if (img_idx >= 128u)
            throw AirError("storage texture index " + std::to_string(img_idx) + " exceeds 128");
        uint32_t handle = AIR2VH_RES_HANDLE(AIR2VH_T_STORAGE, img_idx);
        nir_src_rewrite(&intr->src[0], nir_imm_int(b, (int)handle));
        ctx->table_count[AIR2VH_T_STORAGE] =
            std::max<uint16_t>(ctx->table_count[AIR2VH_T_STORAGE], (uint16_t)(img_idx + 1));
        return true;
    }

    case nir_intrinsic_load_constant: {
        /* Inline constant pool after shader code (spec 07 6.13,
         * panvk_vX_nir_lower_descriptors.c:1024-1052).
         */
        nir_def *offset =
            nir_iadd_imm(b, intr->src[0].ssa, nir_intrinsic_base(intr));
        nir_def *base = nir_load_constant_base_ptr(b, 1, 64);
        nir_def *addr = nir_iadd(b, base, nir_u2u64(b, offset));
        nir_def *val = nir_load_global_constant(
            b, intr->def.num_components, intr->def.bit_size, addr,
            .access = (gl_access_qualifier)(ACCESS_CAN_REORDER | ACCESS_NON_WRITEABLE),
            .align_mul = nir_intrinsic_align_mul(intr),
            .align_offset = nir_intrinsic_align_offset(intr));
        nir_def_replace(&intr->def, val);
        return true;
    }

    default:
        return false;
    }
}

} /* namespace */

void
air2vh_lower_bindings(nir_shader *nir, const AirFrontendResult &fe,
                      uint16_t table_count[AIR2VH_T_COUNT])
{
    memset(table_count, 0, sizeof(uint16_t) * AIR2VH_T_COUNT);

    /* VS stage-private T0 holds vertex attribute descriptors (spec 07 6.8). */
    if (nir->info.stage == MESA_SHADER_VERTEX) {
        for (const auto &at : fe.attributes) {
            if (at.index >= 0) {
                table_count[AIR2VH_T_DRIVER] =
                    std::max<uint16_t>(table_count[AIR2VH_T_DRIVER],
                                       (uint16_t)(at.index + 1));
            }
        }
    }

    /* Seed table_count from used bindings in reflection metadata as well. */
    for (const auto &bd : fe.bindings) {
        if (!bd.used)
            continue;
        if (bd.kind == "ubo" && bd.metal_index >= 0) {
            table_count[AIR2VH_T_BUFFERS] =
                std::max<uint16_t>(table_count[AIR2VH_T_BUFFERS],
                                   (uint16_t)(bd.metal_index + 1));
        } else if (bd.kind == "texture" && bd.metal_index >= 0) {
            table_count[AIR2VH_T_TEXTURES] =
                std::max<uint16_t>(table_count[AIR2VH_T_TEXTURES],
                                   (uint16_t)(bd.metal_index + 1));
        } else if (bd.kind == "storage_texture" && bd.metal_index >= 0) {
            table_count[AIR2VH_T_STORAGE] =
                std::max<uint16_t>(table_count[AIR2VH_T_STORAGE],
                                   (uint16_t)(bd.metal_index + 1));
        } else if ((bd.kind == "sampler" || bd.kind == "constexpr_sampler") &&
                   bd.slot >= 0) {
            table_count[AIR2VH_T_SAMPLERS] =
                std::max<uint16_t>(table_count[AIR2VH_T_SAMPLERS],
                                   (uint16_t)(bd.slot + 1));
        }
    }

    LowerBindingsCtx ctx = {
        .driver_table_ubo = fe.driver_table_ubo,
        .stage = nir->info.stage,
        .table_count = table_count,
    };
    nir_shader_instructions_pass(nir, lower_binding_instr,
                                 nir_metadata_control_flow, &ctx);

    nir->info.num_ubos = table_count[AIR2VH_T_BUFFERS];
    nir->info.num_textures = table_count[AIR2VH_T_TEXTURES];
    nir->info.num_images = table_count[AIR2VH_T_STORAGE];
}

namespace {

struct LowerSysvalsCtx {
    uint64_t shader_present;
    bool no_idvs;
};

static bool
lower_sysval_intr(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
    auto *ctx = static_cast<LowerSysvalsCtx *>(data);
    unsigned ncomp = intr->def.num_components;
    unsigned bs = intr->def.bit_size;
    nir_def *val = nullptr;
    b->cursor = nir_before_instr(&intr->instr);

    switch (intr->intrinsic) {
    case nir_intrinsic_load_core_max_id_arm: {
        uint64_t mask = ctx->shader_present ? ctx->shader_present : 0x01110055ull;
        unsigned max_id = util_last_bit64(mask) - 1;
        val = nir_imm_intN_t(b, max_id, bs);
        break;
    }
    case nir_intrinsic_load_vertex_id: {
        /* When no_idvs is set, bifrost_compile.c:1941 asserts malloc_idvs on
         * load_vertex_id; lower it to load_raw_vertex_id + first_vertex.
         */
        if (!ctx->no_idvs)
            return false;
        nir_def *raw = nir_load_raw_vertex_id(b);
        nir_def *first = emit_sysval_load(
            b, 1, 32, (uint32_t)offsetof(air2vh_sysvals_gfx, first_vertex));
        val = nir_iadd(b, raw, first);
        break;
    }
    case nir_intrinsic_load_num_workgroups:
        val = emit_sysval_load(
            b, ncomp, bs, (uint32_t)offsetof(air2vh_sysvals_cs, num_work_groups));
        break;
    case nir_intrinsic_load_workgroup_size:
        val = emit_sysval_load(
            b, ncomp, bs, (uint32_t)offsetof(air2vh_sysvals_cs, local_group_size));
        break;
    case nir_intrinsic_load_viewport_scale:
        val = emit_sysval_load(
            b, ncomp, bs, (uint32_t)offsetof(air2vh_sysvals_gfx, viewport_scale));
        break;
    case nir_intrinsic_load_viewport_offset:
        val = emit_sysval_load(
            b, ncomp, bs, (uint32_t)offsetof(air2vh_sysvals_gfx, viewport_offset));
        break;
    case nir_intrinsic_load_first_vertex:
    case nir_intrinsic_load_base_vertex:
        val = emit_sysval_load(
            b, ncomp, bs, (uint32_t)offsetof(air2vh_sysvals_gfx, first_vertex));
        break;
    case nir_intrinsic_load_base_instance:
        val = emit_sysval_load(
            b, ncomp, bs, (uint32_t)offsetof(air2vh_sysvals_gfx, base_instance));
        break;
    case nir_intrinsic_load_noperspective_varyings_pan:
        val = emit_sysval_load(
            b, ncomp, bs,
            (uint32_t)offsetof(air2vh_sysvals_gfx, noperspective_varyings));
        break;
    case nir_intrinsic_load_blend_descriptor_pan: {
        uint32_t loc = nir_intrinsic_base(intr);
        if (loc >= 8)
            throw AirError("blend descriptor RT index out of range");
        val = emit_sysval_load(
            b, ncomp, bs,
            (uint32_t)offsetof(air2vh_sysvals_gfx, blend_descs[0]) + loc * 8u);
        break;
    }
    case nir_intrinsic_load_rt_conversion_pan: {
        uint32_t loc = nir_intrinsic_base(intr);
        if (loc >= 8)
            throw AirError("rt_conversion_pan RT index out of range");
        val = emit_sysval_load(
            b, ncomp, bs,
            (uint32_t)offsetof(air2vh_sysvals_gfx, rt_conv[0]) + loc * 4u);
        break;
    }
    case nir_intrinsic_load_layer_id:
    case nir_intrinsic_load_view_index:
        if (b->shader->info.stage == MESA_SHADER_FRAGMENT) {
            val = nir_load_frame_arg_pan(b);
            val = nir_extract_u8_imm(b, nir_u2u32(b, val), 0);
        } else {
            val = nir_imm_int(b, 0);
        }
        break;
    default:
        return false;
    }

    nir_def_replace(&intr->def, val);
    return true;
}

static bool
collect_push_constant(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
    (void)b;
    if (intr->intrinsic != nir_intrinsic_load_push_constant)
        return false;
    auto *plan = static_cast<vh_fau_plan *>(data);
    uint32_t base = nir_intrinsic_base(intr);
    if (base < VH_SYSVAL_PUSH_BASE)
        return false;
    if (!nir_src_is_const(intr->src[0]))
        throw AirError("dynamic push constant offset is not supported");
    uint32_t off = (base - VH_SYSVAL_PUSH_BASE) + nir_src_as_uint(intr->src[0]);
    uint32_t size = (intr->def.bit_size / 8) * intr->def.num_components;
    vh_fau_mark(plan, off, size);
    return false;
}

static bool
remap_push_constant(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
    if (intr->intrinsic != nir_intrinsic_load_push_constant)
        return false;
    auto *plan = static_cast<const vh_fau_plan *>(data);
    uint32_t base = nir_intrinsic_base(intr);
    if (base < VH_SYSVAL_PUSH_BASE)
        return false;
    uint32_t off = (base - VH_SYSVAL_PUSH_BASE) + nir_src_as_uint(intr->src[0]);
    uint32_t remapped = vh_fau_remap(plan, off);

    b->cursor = nir_before_instr(&intr->instr);
    nir_src_rewrite(&intr->src[0], nir_imm_int(b, (int)remapped));
    nir_intrinsic_set_base(intr, 0);
    nir_intrinsic_set_range(intr, 0);
    return true;
}

} /* namespace */

void
air2vh_lower_sysvals_and_fau(nir_shader *nir, uint64_t shader_present,
                             bool no_idvs, vh_fau_plan *plan)
{
    LowerSysvalsCtx ctx = {
        .shader_present = shader_present,
        .no_idvs = no_idvs,
    };
    nir_shader_intrinsics_pass(nir, lower_sysval_intr,
                               nir_metadata_control_flow, &ctx);

    /* Clean up any unused sysval loads before FAU packing (panvk_vX_shader.c:732-748). */
    bool progress = false;
    do {
        progress = false;
        NIR_PASS(progress, nir, nir_opt_copy_prop);
        NIR_PASS(progress, nir, nir_opt_dce);
        NIR_PASS(progress, nir, nir_opt_cse);
        NIR_PASS(progress, nir, nir_opt_constant_folding);
    } while (progress);

    vh_fau_init(plan,
                nir->info.stage == MESA_SHADER_COMPUTE ? VH_SV_CS : VH_SV_GFX,
                nir->info.stage == MESA_SHADER_FRAGMENT ? 1 : 0);

    nir_shader_intrinsics_pass(nir, collect_push_constant,
                               nir_metadata_all, plan);
    if (plan->overflow)
        throw AirError("FAU sysval load overflowed stage sysval struct");
    if (vh_fau_sysval_slots(plan) > VH_FAU_SYSVAL_SLOT_BUDGET)
        throw AirError("FAU sysval slot budget exceeded (" +
                       std::to_string(vh_fau_sysval_slots(plan)) + " > " +
                       std::to_string(VH_FAU_SYSVAL_SLOT_BUDGET) + ")");

    nir_shader_intrinsics_pass(nir, remap_push_constant,
                               nir_metadata_control_flow, plan);
}
