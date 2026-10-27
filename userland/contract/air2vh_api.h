/*
 * air2vh_api.h - air2valhall compiler C API + the shader variant/reflection
 *                struct the Mali command builder compiles against.
 * -------------------------------------------------------------------------
 * Intended repo path: userland/air2valhall/include/air2vh_api.h
 * (compiled by both the compiler library and the Metal plug-in's command
 * builder; the kernel does not include this.)
 *
 * Translates Apple AIR (LLVM bitcode) -> NIR -> Mali Valhall (G715, arch 11)
 * binaries. The front end is air2ir3's AIR->NIR path reused with the Valhall
 * edits in spec 07 section 3.3; the back end is Mesa's Valhall compiler.
 * ORIGINAL interface; air2ir3 (MIT), Mesa (MIT) and panvk were studied as
 * references, not copied.
 *
 * This header is the SHARED CONTRACT: it fixes (a) the compiler entry points
 * and (b) the binding ABI (VH-ABI 1) and `air2vh_program` layout the command
 * builder reads to program the CSF queue. Where this header and spec 06 5.6
 * disagree, spec 07 (and therefore this header) wins [spec 06 section 0].
 *
 * Citations: spec 07-air2valhall.md sections 5, 6, 8; MP: mesa/src/panfrost,
 * A2: air2ir3. Facts carry a spec/file:line; design choices are marked [D].
 */
#ifndef AIR2VH_API_H
#define AIR2VH_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIR2VH_API_VERSION           1u   /* spec 07 section 5 */
#define AIR2VH_ABI_VERSION           1u   /* VH-ABI 1 (binding ABI, section 6) */
#define AIR2VH_FE_ABI_VERSION        6u   /* front-end semantics tracked separately */
#define AIR2VH_CACHE_FORMAT_VERSION  1u   /* spec 07 section 8.3 */

/* ===================================================================== *
 * 1. Binding ABI (VH-ABI 1) - the Metal -> Valhall mapping the builder needs
 *   [spec 07 section 6; MP/compiler/pan_compiler.h:612-640].
 *   A resource handle is (table << 24) | index, table < 64.
 * ===================================================================== */
#define AIR2VH_RES_HANDLE(table, index)   (((uint32_t)(table) << 24) | ((index) & 0xffffffu))

/* The five fixed resource tables per stage (SRT entries) [spec 07 6.2, D3]. */
enum air2vh_table {
    AIR2VH_T_DRIVER   = 0,   /* VS: attributes + vertex buffers; FS(tierA): varying attrs; CS: empty */
    AIR2VH_T_BUFFERS  = 1,   /* Metal constant buffer(n), n<=30 -> load_ubo(handle(T1,n)) */
    AIR2VH_T_TEXTURES = 2,   /* Metal texture(n) sample/read, n<128 */
    AIR2VH_T_SAMPLERS = 3,   /* API/constexpr sampler(n); index 16 = dummy sampler */
    AIR2VH_T_STORAGE  = 4,   /* Metal texture(n) access::write */
    AIR2VH_T_COUNT    = 5,
};
#define AIR2VH_DUMMY_SAMPLER_INDEX   16u  /* fixed in T3 in every stage [D, spec 07 6.2] */
/* SRT array padded to 8 entries (ALIGN_POT(5,4)) = 128 bytes; SRT reg = array|8. */
#define AIR2VH_SRT_ENTRY_COUNT       8u
#define AIR2VH_RESOURCE_ENTRY_BYTES  16u  /* MALI RESOURCE: 56-bit addr, contains_desc, 64-bit size */
#define AIR2VH_DESCRIPTOR_BYTES      32u  /* Sampler/Texture/Buffer/Attribute all 8 dwords */

/* FAU: 64 slots of 8 bytes; blend constants occupy words 0..3 for FS [spec 07 6.11]. */
#define AIR2VH_FAU_SLOTS             64u
#define AIR2VH_FAU_WORDS             128u

/* Scalar base type of a colour output / attribute / varying, encoded directly
 * as Mesa's nir_alu_type values (nir_defines.h:269-289) so the command builder
 * can pass fs.rt[i].base_type straight to pan_blend_shader_key.src0_type
 * (MaliMetal/core/mali_state.c:153). */
#ifndef AIR2VH_BASE_TYPE_DEFINED
#define AIR2VH_BASE_TYPE_DEFINED
enum air2vh_base_type {
    AIR2VH_TYPE_OTHER   = 0,
    AIR2VH_TYPE_INT16   = 18,    /* nir_type_int16   (2 | 16)  */
    AIR2VH_TYPE_UINT16  = 20,    /* nir_type_uint16  (4 | 16)  */
    AIR2VH_TYPE_INT32   = 34,    /* nir_type_int32   (2 | 32)  */
    AIR2VH_TYPE_UINT32  = 36,    /* nir_type_uint32  (4 | 32)  */
    AIR2VH_TYPE_BOOL    = 65,    /* nir_type_bool32  (1 | 32) + 32 */
    AIR2VH_TYPE_FLOAT16 = 144,   /* nir_type_float16 (128 | 16) */
    AIR2VH_TYPE_FLOAT32 = 160,   /* nir_type_float32 (128 | 32) */
};
#endif

/* ===================================================================== *
 * 2. Compiler object + compile request [spec 07 section 5]
 * ===================================================================== */
typedef struct air2vh_compiler air2vh_compiler;

enum air2vh_stage {
    AIR2VH_STAGE_ANY      = 0,   /* no stage check */
    AIR2VH_STAGE_VERTEX   = 1,
    AIR2VH_STAGE_FRAGMENT = 2,
    AIR2VH_STAGE_KERNEL   = 3,
};

/* Variant key flags (part of the cache key, spec 07 section 7). */
enum air2vh_flags {
    AIR2VH_F_NO_IDVS             = (1u << 0), /* key bit0 */
    AIR2VH_F_NONUNIFORM_DISPATCH = (1u << 1), /* key bit1: guard body in all(gid<threads_per_grid) */
};

/* Function-constant value (same layout as air2ir3_constant, A2:air2ir3_api.h:40-45). */
typedef struct air2vh_constant {
    int32_t     index;
    const char *name;
    uint32_t    num_values;
    double      values[4];
} air2vh_constant;

/* Opaque copy of pan_varying_layout, versioned by the compiler id (tier B FS). */
typedef struct air2vh_vs_layout {
    uint32_t    struct_size;
    uint8_t     layout_hash[32];
    uint32_t    blob_size;
    const void *blob;
} air2vh_vs_layout;

typedef struct air2vh_compile_info {
    uint32_t    struct_size;
    const void *air;            /* LLVM bitcode */
    size_t      air_size;
    const char *entry;          /* NULL = the module's only/first entry */
    uint32_t    stage;          /* enum air2vh_stage; ANY = no check */
    uint32_t    num_constants;
    const air2vh_constant *constants;
    uint32_t    flags;          /* enum air2vh_flags */
    const air2vh_vs_layout *vs_layout; /* tier B only; NULL = tier A */
} air2vh_compile_info;

typedef struct air2vh_result {
    const void *code;           /* Valhall binary (section 3.1) */
    size_t      code_size;
    char       *sidecar_json;   /* cache entry (spec 07 8.3); free with air2vh_result_free */
    size_t      sidecar_size;
    struct air2vh_program *program; /* flat struct (section 3.2) */
    air2vh_vs_layout vs_layout_out; /* VS only: the layout this VS produces */
    char        key_hex[65];
    char       *error;          /* NULL on success; "UNSUPPORTED/ERROR: ..." */
    double      compile_ms;
} air2vh_result;

uint32_t    air2vh_api_version(void);
uint32_t    air2vh_abi_version(void);
uint32_t    air2vh_cache_format_version(void);
const char *air2vh_compiler_id(void);   /* sources + Mesa rev + pan_get_compiler_flags(11) + LLVM */

/* gpu_id, core_count, gpu_features come from mali_dev_props (mali_backend_ops.h).
 * Defaults shader_present to 0x01110055 (Husky G715 MC7, max core ID 24) when
 * core_count == 7 on arch 11; override with air2vh_compiler_set_shader_present. */
air2vh_compiler *air2vh_compiler_create(uint64_t gpu_id, uint32_t core_count,
                                        uint64_t gpu_features, char **error);
void air2vh_compiler_destroy(air2vh_compiler *);
void air2vh_compiler_set_shader_present(air2vh_compiler *, uint64_t shader_present);
int  air2vh_compile(air2vh_compiler *, const air2vh_compile_info *, air2vh_result *);
int  air2vh_cache_key(air2vh_compiler *, const air2vh_compile_info *,
                      char key_hex[65], char **error);
void air2vh_result_free(air2vh_result *);
void air2vh_free(void *);

/* Sibling non-AIR entry point owned by this library (spec 07 5, D).
 * Accepts EITHER Mesa's `struct pan_blend_shader_key` (used by MaliMetal's
 * mali_blend_shader_key) OR `air2vh_blend_key` (defined in air2vh_ext.h),
 * discriminated by key_size and struct_size. */
int  air2vh_compile_blend_shader(air2vh_compiler *, const void *key, size_t key_size,
                                 air2vh_result *);

/* ===================================================================== *
 * 3. Output the command builder compiles against
 * ===================================================================== */

/* 3.0 FAU sysval structs (air2vh-private; the builder fills slots by name).
 *   All members 8-byte aligned. [spec 07 6.11]
 *   FS blend.constants MUST be words 0..3 (blend shaders read them). */
typedef struct air2vh_sysvals_gfx {
    struct { float constants[4]; } blend;            /* FS words 0..3 */
    struct { float x, y, z; } viewport_scale;        /* VS */
    struct { float x, y, z; } viewport_offset;       /* VS */
    int32_t  first_vertex;
    int32_t  base_instance;
    uint32_t noperspective_varyings;
    uint64_t blend_descs[8];                          /* FS */
    uint32_t rt_conv[8];                              /* FS fb-fetch conversion words */
    uint64_t buffer_va[31];                           /* device buffer(n): VA + offset */
} air2vh_sysvals_gfx;

typedef struct air2vh_sysvals_cs {
    uint32_t num_work_groups[3];
    uint32_t local_group_size[3];
    uint32_t threads_per_grid[3];
    uint32_t tg_offset[31];
    uint64_t buffer_va[31];
} air2vh_sysvals_cs;

/* 3.1 Binary layout [spec 07 8.1]:
 *   non-IDVS: [code][pad][constant pool]
 *   VS/IDVS (arch 11): [position][position-without-psiz]?[varying]
 *   Alignment: code uploaded 128-byte aligned; FS used with a blend shader
 *   needs its return address 8-byte aligned; all exec code in one 4 GiB window. */
#define AIR2VH_CODE_ALIGN        128u
#define AIR2VH_EXEC_WINDOW_BITS  32u   /* code must not straddle a 4 GiB boundary */

/* 3.2 Flat program struct (stable layout, struct_size first) [spec 07 8.2].
 *   Every field is derived from pan_shader_info or the front end. */
typedef struct air2vh_program {
    uint32_t struct_size, abi_version;
    uint32_t stage;                        /* 1 vs, 2 fs, 3 kernel */

    /* code */
    uint32_t code_size, constant_pool_offset, constant_pool_size;

    /* shader program descriptors (SPD) to build */
    uint32_t entry_offset[3];              /* VS: [0]=pos(points) [1]=pos(triangles) [2]=varying;
                                              FS/CS: [0]=0 */
    uint8_t  spd_count;                    /* VS: 2 (+1 if secondary_enable); FS/CS: 1 */
    uint8_t  work_reg_count[3];            /* register_allocation = (n<=32)?32:64 per SPD */
    uint8_t  _pad_a[3];
    uint64_t preload[3];                   /* SPD cfg.preload.r48_r63 = preload>>48 */
    uint8_t  ftz_fp16, ftz_fp32;
    uint8_t  requires_helper_threads;      /* FS = info.contains_barrier */
    uint8_t  _pad_b;

    /* thread storage (Local Storage descriptor, v11.xml:1797-1807) */
    uint32_t tls_size, wls_size;

    /* FAU: ordered slot sources then promoted immediates [spec 07 6.11] */
    struct {
        uint32_t slots;                    /* value for FAU register bits 63:56 */
        uint32_t reserved_words;           /* inputs.fau.reserved = 2 * sysval slots */
        uint32_t n_src;
        struct { uint8_t sysval, pad[3]; uint32_t byte_offset; } src[AIR2VH_FAU_SLOTS];
        uint32_t n_imm;
        struct { uint32_t word; uint32_t value; } imm[AIR2VH_FAU_WORDS];
    } fau;

    /* resource tables: counts of 32-byte descriptors (index = highest used + 1) */
    uint16_t table_count[AIR2VH_T_COUNT];  /* T0..T4 */

    /* stage-specific */
    struct {
        uint8_t  idvs, secondary_enable, writes_point_size, needs_extended_fifo;
        uint32_t attribute_mask;           /* bit n = Metal [[attribute(n)]] read */
        uint32_t noperspective;
        air2vh_vs_layout layout;           /* the layout this VS produces (tier B) */
    } vs;
    struct {
        uint8_t  reads_frag_coord, reads_point_coord, reads_primitive_id, reads_face;
        uint8_t  can_discard, writes_depth, writes_stencil, writes_coverage;
        uint8_t  sidefx, sample_shading, early_fragment_tests, can_early_z;
        uint8_t  can_fpk, untyped_color_outputs, per_sample_if_msaa, uses_fb_fetch;
        uint32_t outputs_read, fb_fetch_rt_mask;
        uint8_t  earlyzs_lut[24];          /* pan_earlyzs_lut: states[2][2][2][3] */
        uint8_t  uses_ld_var;
        uint16_t varying_desc_count;       /* tier A: V */
        struct { uint8_t index, comps, base_type, written; } rt[8];
    } fs;
    struct {
        uint32_t local_size_hint[3];
        uint8_t  allow_merging_workgroups;
        uint8_t  _pad_c[3];
    } cs;

    uint8_t  contains_barrier, writes_global;
    uint8_t  _pad_d[2];
} air2vh_program;

/* 3.3 Per-binding reflection kinds [spec 07 8.3]. The sidecar JSON carries the
 * same information for tooling/caching; this enum documents the encoding the
 * builder applies per Metal binding. */
enum air2vh_binding_kind {
    AIR2VH_BIND_UBO,                /* handle = AIR2VH_RES_HANDLE(T_BUFFERS, n) */
    AIR2VH_BIND_DEVICE_BUFFER,      /* FAU sysval buffer_va[n] */
    AIR2VH_BIND_THREADGROUP_BUFFER, /* FAU sysval tg_offset[n] */
    AIR2VH_BIND_TEXTURE,            /* handle = AIR2VH_RES_HANDLE(T_TEXTURES, n) */
    AIR2VH_BIND_STORAGE_TEXTURE,    /* handle = AIR2VH_RES_HANDLE(T_STORAGE, n) */
    AIR2VH_BIND_SAMPLER,            /* handle = AIR2VH_RES_HANDLE(T_SAMPLERS, s) */
    AIR2VH_BIND_CONSTEXPR_SAMPLER,  /* {slot, mali_sampler[8]} from the variant */
    AIR2VH_BIND_FB_FETCH,           /* FAU sysval rt_conv[n] */
    AIR2VH_BIND_ATTRIBUTE,          /* T0[n] ATTRIBUTE descriptor (VS) */
};

/* ===================================================================== *
 * 4. Extension and builder out-of-line entry points
 *   (full struct definitions live in air2vh_ext.h and air2vh_builder.h;
 *   declared here so CI's env.sh:air2vh_exports() exports them from
 *   libair2valhall.dylib / libair2valhall.so).
 * ===================================================================== */
typedef struct air2vh_program_ext  air2vh_program_ext;
typedef struct air2vh_varying_link air2vh_varying_link;
typedef struct air2vh_attr_desc    air2vh_attr_desc;

const air2vh_program_ext *air2vh_program_get_ext(const air2vh_program *p);
/* Returns number of links written (or total count if out == NULL && max == 0);
 * returns -1 on error (bad arguments or max < total generic varyings). */
int      air2vh_link_varyings(const air2vh_program *vs, const air2vh_program *fs,
                              air2vh_varying_link *out, uint32_t max,
                              uint32_t *generic_size_B);
uint32_t air2vh_varying_pipe_format(uint8_t alu_type, uint8_t ncomps);

const char *air2vh_sysval_name(uint32_t sysval);
void air2vh_desc_null(uint32_t d[8]);
void air2vh_desc_buffer(uint32_t d[8], uint64_t gpu_va, uint64_t size_bytes);
void air2vh_desc_ubo(uint32_t d[8], uint64_t gpu_va, uint64_t bound_length);
void air2vh_desc_attribute(uint32_t d[8], const air2vh_attr_desc *a);
void air2vh_desc_attribute_unused(uint32_t d[8]);
void air2vh_desc_varying(uint32_t d[8], uint32_t format_hw, uint32_t vs_slot_offset,
                         uint32_t generic_size_B);
void air2vh_desc_dummy_sampler(uint32_t d[8]);
void air2vh_resource_entry(uint32_t e[4], uint64_t gpu_va, uint64_t size_bytes,
                           int contains_descriptors);
int  air2vh_srt_fill(uint32_t srt[AIR2VH_SRT_ENTRY_COUNT][4],
                     const air2vh_program *p,
                     const uint64_t table_va[AIR2VH_T_COUNT],
                     const uint32_t count[AIR2VH_T_COUNT]);
int  air2vh_spd_pack(uint32_t d[8], const air2vh_program *p, unsigned spd_index,
                     uint64_t code_va);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* AIR2VH_API_H */
