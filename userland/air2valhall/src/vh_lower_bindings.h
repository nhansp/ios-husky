/* SPDX-License-Identifier: MIT */
/*
 * vh_lower_bindings.h - Lower AIR-translated NIR bindings & sysvals to VH-ABI 1
 * (spec 07 sections 4, 6.1-6.13).
 */
#ifndef VH_LOWER_BINDINGS_H
#define VH_LOWER_BINDINGS_H

#include <stdint.h>
#include "nir.h"
#include "air2ir3.h"
#include "vh_fau.h"

/* Opaque VS layout blob layout stored inside air2vh_vs_layout.blob (256 bytes):
 * - Offset 0..151 (152 bytes): struct pan_varying_layout (pan_compiler.h:296-303)
 *   so MaliMetal's `((const struct pan_varying_layout *)l->blob)->generic_size_B`
 *   (mali_cs.c:276) works directly at offset 0.
 * - Offset 152..155: magic 'VHVS' (0x56485653).
 * - Offset 156..251 (96 bytes): 24-bit FNV-1a name hashes for VARYING_SLOT_VAR0 + 0..31,
 *   used when Tier-B FS compiles link varyings by name.
 */
struct vh_vs_layout_blob {
    uint8_t  pan_bytes[152];
    uint32_t magic;
    uint8_t  name_hash24[32][3];
};

#define VH_VS_LAYOUT_MAGIC 0x56485653u /* 'VHVS' */

uint32_t air2vh_name_hash24(const char *name);

void air2vh_prepare_pre_nir(nir_shader *nir, AirFrontendResult &fe,
                            uint32_t flags, const air2vh_vs_layout *vs_layout);

/* After pan_preprocess_nir and before pan_postprocess_nir:
 * - Rewrite load_ubo on driver_table_ubo to load_push_constant(VH_SYSVAL_PUSH_BASE + off)
 *   for buffer_va[n], tg_offset[n], and threads_per_grid.
 * - Rewrite app load_ubo(n) to pan_res_handle(AIR2VH_T_BUFFERS, n).
 * - Attach texture_handle = pan_res_handle(AIR2VH_T_TEXTURES, t) and
 *   sampler_handle = pan_res_handle(AIR2VH_T_SAMPLERS, s) to nir_tex_instr.
 * - Rewrite image_store / image_* src[0] to pan_res_handle(AIR2VH_T_STORAGE, n).
 * - Lower load_constant to load_global_constant(load_constant_base_ptr + offset).
 * - Populate table_count[0..4] (T0 for VS attributes; T1..T4 for resources).
 */
void air2vh_lower_bindings(nir_shader *nir, const AirFrontendResult &fe,
                           uint16_t table_count[AIR2VH_T_COUNT]);

/* After pan_postprocess_nir and before pan_shader_compile:
 * - Lower load_core_max_id_arm to imm(util_last_bit64(shader_present) - 1).
 * - When no_idvs is set on VS, lower load_vertex_id to load_raw_vertex_id + first_vertex.
 * - Lower panfrost/compute/graphics sysval intrinsics to load_push_constant
 *   with base = VH_SYSVAL_PUSH_BASE.
 * - Collect used 8-byte words into vh_fau_plan and remap load_push_constant
 *   offsets into consecutive FAU slots with base = 0.
 */
void air2vh_lower_sysvals_and_fau(nir_shader *nir, uint64_t shader_present,
                                  bool no_idvs, vh_fau_plan *plan);

#endif /* VH_LOWER_BINDINGS_H */
