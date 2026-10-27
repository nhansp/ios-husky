/*
 * air2vh_ext.h - additive extensions to the air2valhall C API (VH-ABI 1).
 * -------------------------------------------------------------------------
 * Intended repo path: userland/air2valhall/include/air2vh_ext.h
 *
 * The shared contract (air2vh_api.h) leaves three things the command builder
 * needs without a full C carrier:
 *   (a) per-binding reflection and constexpr-sampler descriptors
 *       (contract 3.3 says "from the variant"; the sidecar JSON has them but a
 *       C builder should not parse JSON at pipeline creation),
 *   (b) the varying tables for tier-A FS<->VS linking by name (spec 07 6.9),
 *   (c) the blend-shader key (contract: "specified with the command builder"),
 * plus one compiler input the contract's create() does not carry
 * (SHADER_PRESENT, for core_max_id inlining as panvk does,
 *  MP/vulkan/panvk_vX_shader.c:507-511).
 *
 * (a)+(b) travel in an extension block placed directly after the
 * air2vh_program struct in the same allocation (air2vh_program.struct_size
 * marks where it starts); air2vh_program_get_ext() finds and validates it.
 * Freed by air2vh_result_free together with the program.
 *
 * Mesa-free: the builder includes this without Mesa headers. Enum values that
 * mirror Mesa (gl_varying_slot, nir_alu_type, pan_varying_section, pipe_format)
 * are passed as plain integers and documented as such.
 * ORIGINAL work; citations as in air2vh_builder.h.
 */
#ifndef AIR2VH_EXT_H
#define AIR2VH_EXT_H

#include <stdint.h>
#include <stddef.h>
#include "air2vh_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIR2VH_EXT_MAGIC          0x31585856u   /* "VVX1" little-endian */
#define AIR2VH_EXT_VERSION        1u
#define AIR2VH_EXT_MAX_BINDINGS   256u          /* up to 31 buf + 128 tex + 16 samp + 31 tg + 31 attr */
#define AIR2VH_EXT_MAX_CSAMPLERS  16u           /* Metal: <= 16 samplers incl. constexpr */
#define AIR2VH_EXT_MAX_VARYINGS   40u           /* >= PAN_MAX_VARYINGS (MAX_VARYING + 5) */
#define AIR2VH_EXT_MAX_ATTRIBUTES 31u
#define AIR2VH_EXT_NAME_LEN       64u

/* Scalar base type of a colour output / attribute / varying as the AIR
 * function declares it, using Mesa's nir_alu_type numbering
 * (defined in air2vh_api.h so MaliMetal's mali_state.c:153 and air2vh_ext.h
 * agree on the exact integer values). */
#ifndef AIR2VH_BASE_TYPE_DEFINED
#define AIR2VH_BASE_TYPE_DEFINED
enum air2vh_base_type {
    AIR2VH_TYPE_OTHER   = 0,
    AIR2VH_TYPE_INT16   = 18,
    AIR2VH_TYPE_UINT16  = 20,
    AIR2VH_TYPE_INT32   = 34,
    AIR2VH_TYPE_UINT32  = 36,
    AIR2VH_TYPE_BOOL    = 65,
    AIR2VH_TYPE_FLOAT16 = 144,
    AIR2VH_TYPE_FLOAT32 = 160,
};
#endif

/* One Metal resource binding of the variant (spec 07 8.3 "bindings[]"). */
typedef struct air2vh_binding {
    uint8_t  kind;          /* enum air2vh_binding_kind (air2vh_api.h 3.3)   */
    uint8_t  used;          /* the specialised code reads it                  */
    uint8_t  present;       /* Metal reflection lists it (present:false + used =
                               bind-through, A2/BINDING-ABI.md:102)            */
    uint8_t  table;         /* AIR2VH_T_* for table kinds, 0xff otherwise     */
    int16_t  metal_index;   /* [[buffer/texture/sampler/threadgroup(n)]], RT for
                               fb_fetch, S# slot for constexpr samplers        */
    uint8_t  sysval;        /* enum air2vh_sysval for FAU kinds, else 0       */
    uint8_t  access;        /* textures: 0 sample, 1 read, 2 write            */
    uint32_t handle;        /* AIR2VH_RES_HANDLE(table, index) or 0           */
    int32_t  minimum_bytes; /* ubo/device: AIR air.buffer_size, -1 = none     */
    uint32_t fau_byte_offset; /* FAU kinds: byte offset in the sysval struct  */
} air2vh_binding;

/* A constexpr sampler of the variant: the builder writes mali_sampler into
 * T3[slot] on every pipeline bind (spec 07 6.6). int_border replaces words
 * 4..7 when the bound texture is an integer format [D, spec 07 6.6 border]. */
typedef struct air2vh_constexpr_sampler {
    uint32_t slot;
    uint32_t _pad;
    uint64_t unique_id;       /* A2/BINDING-ABI.md 3.5 / CACHE-FORMAT.md:91 */
    uint64_t air_word;        /* AIR sampler state word 0                   */
    uint32_t mali_sampler[8]; /* v11.xml:1393-1421                          */
    uint32_t int_border[4];
} air2vh_constexpr_sampler;

/*
 * One varying slot.
 *   VS: the layout this VS produces (pan_build_varying_layout_compact):
 *       offset = byte offset in its section, generic_size_B in the block.
 *   FS: info.varyings.formats; array index == LD_VAR descriptor index ==
 *       T0[index] in tier A (MP/compiler/pan_nir_lower_fs_inputs.c:130-133).
 * name is the Metal varying name (user(...)/generated(...)), "" for builtins.
 * location: gl_varying_slot; section: pan_varying_section (0 position,
 * 1 attribs, 2 special, 3 generic, MP/compiler/pan_compiler.h:219-225);
 * alu_type: nir_alu_type (base | bit size); empty slots have alu_type 0.
 */
typedef struct air2vh_varying_slot {
    char     name[AIR2VH_EXT_NAME_LEN];
    uint8_t  location, section, alu_type, ncomps;
    int16_t  offset;          /* VS only; -1 when unknown                   */
    uint8_t  interp;          /* 0 perspective, 1 flat, 2 no_perspective    */
    uint8_t  sampling;        /* 0 center, 1 centroid, 2 sample             */
} air2vh_varying_slot;

typedef struct air2vh_attribute {
    uint8_t index, comps, bits, base_type;   /* base_type: enum air2vh_base_type */
} air2vh_attribute;

struct air2vh_program_ext {
    uint32_t magic, version, size;

    uint32_t n_bindings;
    air2vh_binding bindings[AIR2VH_EXT_MAX_BINDINGS];

    uint32_t n_constexpr_samplers;
    uint32_t constexpr_sampler_mask;           /* bit s = T3[s] is constexpr */
    uint8_t  constexpr_set_id[8];              /* CACHE-FORMAT.md 3.2 set id  */
    air2vh_constexpr_sampler constexpr_samplers[AIR2VH_EXT_MAX_CSAMPLERS];

    uint32_t n_varyings;
    uint16_t generic_size_B;                   /* VS layout; 0 for FS        */
    uint16_t _pad0;
    air2vh_varying_slot varyings[AIR2VH_EXT_MAX_VARYINGS];

    uint32_t n_attributes;
    air2vh_attribute attributes[AIR2VH_EXT_MAX_ATTRIBUTES];

    uint32_t threadgroup_bytes;      /* static threadgroup memory (wls_size) */
    uint32_t constant_data_bytes;
    uint32_t requires_no_attachment; /* bit n: refuse a pipeline with colour
                                        attachment n (A2/BINDING-ABI.md 3.7) */
    uint8_t  uses_dummy_sampler;     /* T3[16] must hold the dummy sampler   */
    uint8_t  tier_b;                 /* FS compiled against a VS layout       */
    uint8_t  _pad1[2];
};

/* The extension block of a program returned by air2vh_compile, or NULL
 * (wrong magic/version/size, e.g. a program from an older library). */
const air2vh_program_ext *air2vh_program_get_ext(const air2vh_program *p);

/* ---------------------------------------------------------------------- *
 * Tier-A varying linking (Mesa-free; src/vh_desc.c)
 * For each FS varying slot i in the GENERIC section: find the VS varying
 * with the same NAME (Metal links by name, A2/BINDING-ABI.md:139), take the
 * VS slot's offset, and form the format from the FS base type and the VS
 * bit size / component count, exactly as panvk's emit_varying_descs does
 * (MP/vulkan/csf/panvk_vX_cmd_draw.c:470-494). Unmatched -> matched = 0
 * (the builder writes a format-NONE descriptor, as panvk does).
 * ---------------------------------------------------------------------- */
struct air2vh_varying_link {
    uint32_t fs_index;        /* T0 index in the FS SRT                     */
    uint32_t vs_offset;       /* -> air2vh_desc_varying(vs_slot_offset)      */
    uint8_t  alu_type;        /* -> air2vh_varying_pipe_format()             */
    uint8_t  ncomps;
    uint8_t  matched;
    uint8_t  _pad;
};

/* Returns the number of links written (or total count if out == NULL && max == 0),
 * or -1 on error (including bad arguments or when max is too small for all generic varyings).
 * *generic_size_B receives the VS layout's generic section size. */
int air2vh_link_varyings(const air2vh_program *vs, const air2vh_program *fs,
                         air2vh_varying_link *out, uint32_t max,
                         uint32_t *generic_size_B);

/* pipe_format (Mesa enum value) of a varying in memory: Mesa
 * pan_varying_format(alu_type, ncomps) (MP/compiler/pan_compiler.h:352-353).
 * Implemented in the compiler library (needs Mesa). */
uint32_t air2vh_varying_pipe_format(uint8_t alu_type, uint8_t ncomps);

/* ---------------------------------------------------------------------- *
 * Compiler inputs beyond air2vh_compiler_create
 * ---------------------------------------------------------------------- */
/* SHADER_PRESENT of the device (mali_dev_props.shader_present). Inlined as
 * load_core_max_id_arm = last_bit(shader_present) - 1, as panvk does
 * (MP/vulkan/panvk_vX_shader.c:507-511). Default when never set: 0x01110055
 * on arch 11 with core_count == 7 (husky, max id 24), or (1ULL << core_count) - 1
 * otherwise. Not key material: the cache key names the gpu_prod_id; set it
 * before the first compile if a non-husky sparse mask is used. */
void air2vh_compiler_set_shader_present(air2vh_compiler *, uint64_t shader_present);

/* ---------------------------------------------------------------------- *
 * Blend shaders (air2vh_compile_blend_shader key, spec 07 5 [D])
 * The FS code is blend-independent (MP/compiler/pan_nir_lower_fs_outputs.c:
 * 183-216); when the Blend descriptor cannot do an equation in fixed
 * function (MP/vulkan/panvk_vX_blend.c:211-260) the builder compiles one of
 * these and points the Blend descriptor's shader PC at it. Blend shaders read
 * the blend constants from FAU words 0..3 (panvk_vX_blend.c:42-46), which is
 * why every FS reserves them (air2vh_sysvals_gfx.blend, C11).
 * Upload rules: 16-byte aligned, same 4 GiB window as the FS
 * (panvk_vX_blend.c:139-145).
 * Note: air2vh_compile_blend_shader accepts EITHER this Mesa-free
 * air2vh_blend_key struct OR Mesa's struct pan_blend_shader_key.
 * ---------------------------------------------------------------------- */
typedef struct air2vh_blend_key {
    uint32_t struct_size;
    uint32_t pipe_format;     /* Mesa enum pipe_format of the attachment view   */
    uint8_t  rt;              /* colour attachment 0..7                         */
    uint8_t  nr_samples;      /* raster sample count (1, 2, 4, 8)               */
    uint8_t  src0_type;       /* enum air2vh_base_type of the FS output (rt[].base_type) */
    uint8_t  src1_type;       /* dual source: type of index(1) output, 0 = none */
    uint8_t  blending_enabled;/* MTLRenderPipelineColorAttachmentDescriptor     */
    uint8_t  rgb_op, alpha_op;/* MTLBlendOperation: 0 add 1 sub 2 rev-sub 3 min 4 max */
    uint8_t  src_rgb, dst_rgb, src_alpha, dst_alpha; /* MTLBlendFactor 0..18    */
    uint8_t  write_mask;      /* MTLColorWriteMask: R 8, G 4, B 2, A 1          */
    uint8_t  alpha_to_one;    /* MTLRenderPipelineDescriptor.alphaToOneEnabled  */
    uint8_t  _pad[2];
} air2vh_blend_key;

#ifdef __cplusplus
}
#endif
#endif /* AIR2VH_EXT_H */
