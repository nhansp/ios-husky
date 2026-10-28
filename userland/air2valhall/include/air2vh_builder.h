/*
 * air2vh_builder.h - what the Mali command builder needs to TURN an
 *                    air2vh_program into hardware state (VH-ABI 1).
 * -------------------------------------------------------------------------
 * Intended repo path: userland/air2valhall/include/air2vh_builder.h
 *
 * Additive to the shared contract air2vh_api.h.
 * Mesa-free: plain C99, no genxml, no NIR, so the Metal plug-in's command
 * builder can include it without pulling in Mesa headers. Implemented in
 * src/vh_desc.c and src/vh_sampler.c (also Mesa-free, unit-tested by
 * tests/test_vh_standalone.c on the host).
 *
 * ORIGINAL code. Bit positions are taken from Mesa's Valhall v11 register /
 * descriptor XML (MIT) and cited as  v11.xml:<line>  =
 *   gpuref/mesa/src/panfrost/genxml/v11.xml (Mesa HEAD 1f9035d).
 * Usage rules cite panvk (MIT) as  MP/vulkan/<file>:<line>.  Nothing is copied.
 * Claim ids (C2, C6, C7, C8, C11, C13) refer to
 *   artifacts/research/gpu-spec/99-claims-ledger.json (all "confirmed").
 */
#ifndef AIR2VH_BUILDER_H
#define AIR2VH_BUILDER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "air2vh_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== *
 * 1. FAU sysvals
 * ===================================================================== */

/*
 * Identity of a FAU sysval. air2vh_program.fau.src[k].sysval holds one of
 * these (the member whose first byte the 8-byte slot starts at) and
 * src[k].byte_offset is the slot's offset inside the stage's sysval struct:
 *   stage 1/2 (VS/FS) -> air2vh_sysvals_gfx,  stage 3 (kernel) -> air2vh_sysvals_cs
 * [spec 07 6.11; struct layouts are the contract's, air2vh_api.h 3.0].
 * The builder only needs byte_offset; the id is for validation/tooling.
 */
enum air2vh_sysval {
    AIR2VH_SV_NONE = 0,
    /* air2vh_sysvals_gfx */
    AIR2VH_SV_BLEND_CONSTANTS = 1,     /* FS, always slots 0..1 (C11)        */
    AIR2VH_SV_VIEWPORT_SCALE,          /* VS: nir_lower_viewport_transform   */
    AIR2VH_SV_VIEWPORT_OFFSET,         /* VS                                  */
    AIR2VH_SV_FIRST_VERTEX,            /* VS: load_first_vertex/base_vertex  */
    AIR2VH_SV_BASE_INSTANCE,           /* VS                                  */
    AIR2VH_SV_NOPERSPECTIVE_VARYINGS,  /* VS: pan_nir_lower_noperspective_vs */
    AIR2VH_SV_BLEND_DESCS,             /* FS: load_blend_descriptor_pan(rt)  */
    AIR2VH_SV_RT_CONV,                 /* FS: load_rt_conversion_pan(rt)     */
    AIR2VH_SV_GFX_BUFFER_VA,           /* VS/FS: device buffer(n) VA         */
    /* air2vh_sysvals_cs */
    AIR2VH_SV_NUM_WORK_GROUPS,
    AIR2VH_SV_LOCAL_GROUP_SIZE,
    AIR2VH_SV_THREADS_PER_GRID,
    AIR2VH_SV_TG_OFFSET,               /* threadgroup(n) byte offset          */
    AIR2VH_SV_CS_BUFFER_VA,            /* kernel device buffer(n) VA          */
    AIR2VH_SV_COUNT
};

/* Name used in the sidecar JSON ("buffer_va", "blend.constants", ...). */
const char *air2vh_sysval_name(uint32_t sysval);

/* sizeof the sysval struct a program's FAU sources index into. */
static inline size_t air2vh_sysvals_size(const air2vh_program *p)
{
    return p->stage == AIR2VH_STAGE_KERNEL ? sizeof(air2vh_sysvals_cs)
                                           : sizeof(air2vh_sysvals_gfx);
}

/*
 * Build the FAU buffer for one draw/dispatch: slot k <- 8 bytes of the
 * builder's sysval struct at src[k].byte_offset, then the compiler-promoted
 * immediates (32-bit words) on top. Same merge order as
 * MP/vulkan/panvk_vX_cmd_push_constant.c:44-75 (sysvals, then immediates).
 * Returns the slot count (>= 0) to put in FAU_n bits 63:56 on success, or -1
 * on error (NULL pointers, malformed program, out-of-bounds immediate, or
 * sysvals buffer smaller than required). A valid program with 0 FAU slots
 * returns 0, distinct from error (-1).
 * `fau` must hold AIR2VH_FAU_SLOTS entries and be uploaded 8-byte
 * aligned, CLEANED to DRAM (non-coherent GPU, HG:41) before the job runs.
 */
static inline int32_t air2vh_fau_fill(const air2vh_program *p,
                                      const void *sysvals, size_t sysvals_size,
                                      uint64_t fau[AIR2VH_FAU_SLOTS])
{
    if (!p || !fau || p->fau.slots > AIR2VH_FAU_SLOTS || p->fau.n_src > AIR2VH_FAU_SLOTS ||
        p->fau.n_imm > AIR2VH_FAU_WORDS)
        return -1;
    if (p->fau.slots > 0 && (!sysvals || sysvals_size < air2vh_sysvals_size(p)))
        return -1;
    memset(fau, 0, sizeof(uint64_t) * AIR2VH_FAU_SLOTS);
    for (uint32_t k = 0; k < p->fau.n_src; k++) {
        uint32_t off = p->fau.src[k].byte_offset;
        if (off + 8u > sysvals_size)
            return -1;
        memcpy(&fau[k], (const uint8_t *)sysvals + off, 8);
    }
    for (uint32_t i = 0; i < p->fau.n_imm; i++) {
        uint32_t w = p->fau.imm[i].word;
        if (w >= 2u * p->fau.slots)
            return -1;
        memcpy((uint8_t *)fau + 4u * w, &p->fau.imm[i].value, 4);
    }
    return (int32_t)p->fau.slots;
}

/* Helper to check FAU fill status explicitly with output pointer:
 * returns 0 on success, or -1 on error. */
static inline int air2vh_fau_fill_status(const air2vh_program *p,
                                         const void *sysvals, size_t sysvals_size,
                                         uint64_t fau[AIR2VH_FAU_SLOTS],
                                         uint32_t *out_slots)
{
    int32_t rc = air2vh_fau_fill(p, sysvals, sysvals_size, fau);
    if (rc < 0)
        return -1;
    if (out_slots)
        *out_slots = (uint32_t)rc;
    return 0;
}

/* ===================================================================== *
 * 2. Register values (COMPUTE/IDVS SR maps: v11.xml:1087-1167, claim
 *    C6-v11-sr-maps; tagged-pointer encodings: claim C8)
 * ===================================================================== */

/* SRT_n = srt_gpu_va | res_count, res_count = ALIGN_POT(5,4) = 8; the SRT
 * array is 8 x 16-byte RESOURCE entries, 64-byte aligned
 * [MP/vulkan/panvk_vX_cmd_desc_state.c:263-311, v11.xml:2355, claim C13]. */
static inline uint64_t air2vh_srt_reg(uint64_t srt_gpu_va)
{
    return srt_gpu_va | AIR2VH_SRT_ENTRY_COUNT;
}

/* FAU_n = fau_gpu_va | (slots << 56) [MP/vulkan/csf/panvk_vX_cmd_dispatch.c:225-229, C6]. */
static inline uint64_t air2vh_fau_reg(uint64_t fau_gpu_va, const air2vh_program *p)
{
    return (fau_gpu_va & 0x00ffffffffffffffULL) | ((uint64_t)p->fau.slots << 56);
}

/* WG_SIZE = "Compute size workgroup" (v11.xml:2358-2363): size-1 per axis in
 * 10-bit fields at 0/10/20, bit 31 allow-merging (always 0 in v1, spec 07 6.12). */
static inline uint32_t air2vh_wg_size_reg(uint32_t x, uint32_t y, uint32_t z, int allow_merge)
{
    return ((x - 1u) & 0x3ffu) | (((y - 1u) & 0x3ffu) << 10) |
           (((z - 1u) & 0x3ffu) << 20) | (allow_merge ? (1u << 31) : 0u);
}

/* ===================================================================== *
 * 3. Descriptors (all 8 dwords, 32-byte aligned) and SRT entries
 * ===================================================================== */

/* Mali "Descriptor Type" (v11.xml:42-51). */
enum air2vh_mali_desc_type {
    AIR2VH_MALI_DESC_NULL = 0, AIR2VH_MALI_DESC_SAMPLER = 1, AIR2VH_MALI_DESC_TEXTURE = 2,
    AIR2VH_MALI_DESC_ATTRIBUTE = 5, AIR2VH_MALI_DESC_SHADER = 8, AIR2VH_MALI_DESC_BUFFER = 9,
};

/* NULL descriptor (v11.xml:1180-1182): unbound T1/T2/T4 slots and vertex
 * buffers [D, spec 07 6.2; MP/vulkan/csf/panvk_vX_cmd_draw.c:385-388]. */
void air2vh_desc_null(uint32_t d[8]);

/* BUFFER, simple (v11.xml:1293-1311): 64-bit size split Size/Size hi. */
void air2vh_desc_buffer(uint32_t d[8], uint64_t gpu_va, uint64_t size_bytes);
/* T1 constant buffer: size rounded up to 16 [D, spec 07 6.3;
 * MP/vulkan/panvk_vX_cmd_desc_state.c:249-252]. Unbound -> air2vh_desc_null. */
void air2vh_desc_ubo(uint32_t d[8], uint64_t gpu_va, uint64_t bound_length);

/* ATTRIBUTE (v11.xml:1184-1202). format_hw = Mali "Pixel Format" (22 bits),
 * from libpan GENX(pan_format_from_pipe_format)(f)->hw in the builder. */
enum air2vh_attr_type {             /* v11.xml:7-13 */
    AIR2VH_ATTR_1D = 1, AIR2VH_ATTR_1D_POT_DIVISOR = 2, AIR2VH_ATTR_1D_NPOT_DIVISOR = 3,
    AIR2VH_ATTR_VERTEX_PACKET = 6,
};
struct air2vh_attr_desc {
    uint32_t format_hw;     /* 22 bits */
    uint8_t  attribute_type;/* enum air2vh_attr_type */
    uint8_t  offset_enable; /* per-vertex attributes: 1 (panvk_vX_cmd_draw.c:300-304) */
    uint8_t  table;         /* 0 = T0 (the VH-ABI 1 vertex buffers live at T0[32+b]) */
    uint8_t  per_instance;  /* frequency: 0 vertex, 1 instance (v11.xml:484-487) */
    uint8_t  divisor_r, divisor_e;
    int32_t  offset;
    uint32_t buffer_index;  /* VH-ABI 1: 32 + Metal vertex-buffer index */
    uint32_t stride;
    uint32_t divisor_d;
};
void air2vh_desc_attribute(uint32_t d[8], const air2vh_attr_desc *a);

/* T0[n] for a Metal attribute the shader reads but the vertex descriptor
 * leaves unset: table 17 (invalid -> OOB, reads return 0), R16F/RGBA format
 * [MP/vulkan/csf/panvk_vX_cmd_draw.c:358-364; v11.xml:248 (R16F=199), :292]. */
void air2vh_desc_attribute_unused(uint32_t d[8]);

/* Varying ATTRIBUTE for FS tier A (LD_VAR), one per FS input index:
 * VERTEX_PACKET, table 61, offset 1024 + vs_slot_offset, attribute stride =
 * generic_size_B, packet stride = generic_size_B + 16, buffer index 0 (< v12)
 * [MP/vulkan/csf/panvk_vX_cmd_draw.c:461-509, spec 07 6.9]. */
void air2vh_desc_varying(uint32_t d[8], uint32_t format_hw, uint32_t vs_slot_offset,
                         uint32_t generic_size_B);

/* SAMPLER, default state with clamp_integer_array_indices = false: the T3[16]
 * dummy sampler of every stage [spec 07 6.2; panvk_vX_cmd_draw.c:368-370]. */
void air2vh_desc_dummy_sampler(uint32_t d[8]);

/* 16-byte RESOURCE entry of the SRT (v11.xml:2365-2369): 56-bit address,
 * "Contains descriptors" at word1 bit 24, 64-bit size in bytes. */
void air2vh_resource_entry(uint32_t e[4], uint64_t gpu_va, uint64_t size_bytes,
                           int contains_descriptors);

/*
 * Fill the 8-entry SRT for one stage: entry t (t < 5) points at table t with
 * size 32 * count[t]; a zero count or zero VA gives the all-zero entry;
 * entries 5..7 are zero [spec 07 6.2 D; panvk_vX_cmd_desc_state.c:300-305].
 * count[t] must be >= p->table_count[t] (the builder may add more, e.g. the
 * vertex buffers at T0[32+b], which the compiler cannot know).
 * Returns 0, or -1 when a count is below the program's requirement.
 */
int air2vh_srt_fill(uint32_t srt[AIR2VH_SRT_ENTRY_COUNT][4],
                    const air2vh_program *p,
                    const uint64_t table_va[AIR2VH_T_COUNT],
                    const uint32_t count[AIR2VH_T_COUNT]);

/* ===================================================================== *
 * 4. SHADER_PROGRAM (SPD) descriptor (v11.xml:1775-1788)
 * ===================================================================== */

int air2vh_spd_pack(uint32_t d[8], const air2vh_program *p, unsigned spd_index,
                    uint64_t code_va);

/* VS SPD order (entry_offset[] / work_reg_count[] / preload[] index). */
enum air2vh_vs_spd {
    AIR2VH_VS_SPD_POS_POINTS = 0,     /* position shader, writes point size   */
    AIR2VH_VS_SPD_POS_TRIANGLES = 1,  /* position shader with psiz NOPed out  */
    AIR2VH_VS_SPD_VARYING = 2,        /* IDVS varying shader (secondary)      */
};

#ifdef __cplusplus
}
#endif
#endif /* AIR2VH_BUILDER_H */
