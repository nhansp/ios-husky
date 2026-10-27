/*
 * mali_core.h - hardware-facing core of the MaliMetal plug-in (plain C, no Metal types)
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/core/mali_core.h
 *
 * This is the part of the plug-in that knows Mali-G715 (Valhall CSF, arch 11)
 * and Mesa's libraries. The Objective-C classes (MaliMTLDevice, the encoders,
 * the resources) only translate Metal API state into the plain structs below
 * and call these functions; nothing in here includes Metal or Foundation, so
 * the whole layer compiles and is unit-tested on a Linux host against the
 * generated genxml v11 packers (tests/host/test_core.c).
 *
 * ORIGINAL work. Mesa (MIT) and panvk were studied as references; every
 * sequence cites the Mesa file:line it follows, using spec 06 section 0's keys:
 *     M:   mesa/src/panfrost        (Mesa 27.0.0-devel, HEAD 1f9035d)
 *     MV:  M:vulkan/csf             (panvk CSF driver, worked example only)
 *     HWR: M:compiler/kraid/hw_runner/hw_runner_vX.c
 *     HG:  artifacts/xnu-12377/bsd/dev/husky_gpu.c
 *     S06: gpu-spec/06-mali-emit-path.md      S07: gpu-spec/07-air2valhall.md
 *
 * Metal enum values appear here as plain integers, always with the Metal name
 * next to them; the Objective-C side static-asserts the ones it passes.
 *
 * MEMORY MODEL. Everything the GPU reads that the plug-in builds (command
 * stream chunks, descriptors, FAU, TLS, tiler context, FBD) lives in
 * "arenas": bump allocators over IOGPU buffers the Objective-C side creates
 * with a CPU write-combined mapping (S06 6.3: NC CPU view, GPU AttrIndx 0).
 * The kernel cleans every resource listed in a segment's resource list
 * (spec 03 section 5.4), so arena chunks are listed at commit.
 */
#ifndef MALI_CORE_H
#define MALI_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "air2vh_api.h"        /* contract: shader program + sysval structs  */
#include "air2vh_ext.h"
#include "mali_backend_ops.h"  /* contract: mali_dev_props_t, MALI_CS_*      */

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== *
 * 0. Constants
 * ===================================================================== */
#define MALI_ARCH                 11u
#define MALI_MAX_RTS              8u
#define MALI_MAX_BUFFERS          31u      /* Metal buffer(0..30) [S07 6.2 T1] */
#define MALI_MAX_TEXTURES         16u      /* texture(0..15)      [S07 6.2 T2] */
#define MALI_MAX_SAMPLERS         16u      /* sampler(0..15)      [S07 6.2 T3] */
#define MALI_DUMMY_SAMPLER        AIR2VH_DUMMY_SAMPLER_INDEX   /* T3[16]     */
#define MALI_MAX_VATTRS           31u
#define MALI_DESC_SIZE            32u      /* every descriptor is 32 B [S07 6.1] */
#define MALI_SRT_ENTRIES          8u       /* ALIGN_POT(5,4)           [S07 6.2] */
#define MALI_SRT_BYTES            (MALI_SRT_ENTRIES * AIR2VH_RESOURCE_ENTRY_BYTES)
#define MALI_FAU_BYTES_MAX        (AIR2VH_FAU_SLOTS * 8u)

/* Scoreboard plan for the single-CS design (decision D-1 in MaliPluginABI.h).
 * v11 registers: LS (load/store) slot 0, endpoint slot 1, deferred slot 2, a
 * second wait slot 3 [MV:panvk_cmd_buffer.h:244-262 uses the same ids] */
#define MALI_SB_LS                0u
#define MALI_SB_EP                1u
#define MALI_SB_VT                2u       /* HEAP_OPERATION(VT completed) signal */
#define MALI_SB_FRAG              3u       /* FINISH_FRAGMENT signal              */
#define MALI_SB_MASK(n)           (1u << (n))

/* ===================================================================== *
 * 1. Context
 * ===================================================================== */
typedef struct mali_ctx {
    mali_dev_props_t props;      /* contract mali_backend_ops.h section 1      */
    unsigned arch;               /* 11                                          */
    const void *model;           /* const struct pan_model *                    */
    uint32_t nr_regs;            /* props.cs_reg_count (96)                     */
    uint32_t nr_kernel_regs;     /* last 4 are the kernel's [S06 2]             */
    uint32_t tile_rt_budget_B;   /* pan_query_optimal_tib_size                  */
    uint32_t tile_z_budget_B;    /* pan_query_optimal_z_tib_size                */
    uint32_t tiler_max_levels;
    uint32_t tiler_chunk_size;   /* heap chunk size the kernel creates [S06 5.4] */
    uint32_t core_id_range;      /* WLS/TLS sizing uses this, not the core count */
} mali_ctx_t;

/* Fill from the device properties (read from the accelerator's registry
 * properties, or the husky ground truth when absent). 0 on success. */
int  mali_ctx_init(mali_ctx_t *ctx, const mali_dev_props_t *props);
/* The husky ground truth of mali_backend_ops.h's header comment (S06 section 2). */
void mali_dev_props_husky_default(mali_dev_props_t *props);

/* ===================================================================== *
 * 2. GPU-visible memory
 * ===================================================================== */
typedef struct mali_gpu_ptr {
    void    *cpu;
    uint64_t gpu;
} mali_gpu_ptr_t;

typedef struct mali_arena_ops {
    /* Return a fresh, zeroed chunk of at least min_size bytes (page aligned,
     * GPU-mapped, CPU write-combined). The owner keeps it alive until the
     * command buffer that used it has completed. */
    bool (*new_chunk)(void *ctx, uint32_t min_size, mali_gpu_ptr_t *out, uint32_t *size_out);
} mali_arena_ops_t;

typedef struct mali_arena {
    const mali_arena_ops_t *ops;
    void     *ops_ctx;
    uint8_t  *cpu;               /* current chunk                              */
    uint64_t  gpu;
    uint32_t  size, used;
    uint64_t  total_bytes;       /* all chunks handed out (statistics)         */
    uint32_t  chunk_count;
} mali_arena_t;

void           mali_arena_init(mali_arena_t *a, const mali_arena_ops_t *ops, void *ops_ctx);
/* align must be a power of two; returns {NULL,0} when no chunk can be added.
 * The memory is zero (chunks arrive zeroed and are never reused in place). */
mali_gpu_ptr_t mali_arena_alloc(mali_arena_t *a, uint32_t size, uint32_t align);
/* Order-of-writes barrier before the doorbell: the arenas are NC, so dsb sy. */
void           mali_cpu_barrier(void);

/* ===================================================================== *
 * 3. Pixel formats  (mali_formats.c)
 * ===================================================================== */
enum mali_fmt_flags {
    MALI_FMT_COLOR_RT   = 1u << 0,   /* usable as a render target here         */
    MALI_FMT_SAMPLED    = 1u << 1,
    MALI_FMT_STORAGE    = 1u << 2,   /* image_store from a compute kernel      */
    MALI_FMT_BLENDABLE  = 1u << 3,   /* fixed-function blend works             */
    MALI_FMT_DEPTH      = 1u << 4,
    MALI_FMT_STENCIL    = 1u << 5,
    MALI_FMT_SRGB       = 1u << 6,
    MALI_FMT_INTEGER    = 1u << 7,
    MALI_FMT_FLOAT      = 1u << 8,
    MALI_FMT_CPU_LINEAR = 1u << 9,   /* replaceRegion/getBytes layout is plain */
};
typedef struct mali_fmt_info {
    int      pipe_format;        /* enum pipe_format (Mesa util/format/u_formats.h) */
    uint32_t bytes_per_pixel;    /* 0 for formats with no per-pixel layout          */
    uint32_t flags;              /* enum mali_fmt_flags                              */
} mali_fmt_info_t;
/* MTLPixelFormat -> info. Returns false for formats the v1 plug-in refuses
 * (compressed, XR, YCbCr, packed 24-bit depth); info is zeroed then. */
bool mali_fmt_lookup(uint32_t mtl_pixel_format, mali_fmt_info_t *out);
/* MTLVertexFormat -> pipe_format for Attribute descriptors (0 = unsupported). */
int  mali_vertex_fmt_to_pipe(uint32_t mtl_vertex_format);

/* ===================================================================== *
 * 4. Textures  (mali_texture.c)
 *   Linear layouts only in v1 (modifier DRM_FORMAT_MOD_LINEAR): the display
 *   path is linear until AFBC scanout is specified [S06 section 11 item 9].
 *   The layout itself (level offsets, row/array strides) is computed by Mesa's
 *   pan_image_layout_init so GPU descriptors and CPU copies share one source.
 * ===================================================================== */
typedef struct mali_texture mali_texture_t;     /* opaque, ref-counted image + view */

enum mali_tex_type {                            /* values of MTLTextureType     */
    MALI_TEX_1D = 0, MALI_TEX_1D_ARRAY = 1, MALI_TEX_2D = 2, MALI_TEX_2D_ARRAY = 3,
    MALI_TEX_2D_MS = 4, MALI_TEX_CUBE = 5, MALI_TEX_CUBE_ARRAY = 6, MALI_TEX_3D = 7,
    MALI_TEX_2D_MS_ARRAY = 8, MALI_TEX_BUFFER = 9,
};

typedef struct mali_texture_params {
    uint32_t mtl_pixel_format;
    uint32_t mtl_texture_type;   /* enum mali_tex_type                          */
    uint32_t width, height, depth;
    uint32_t array_length;       /* MTLTextureDescriptor.arrayLength (>= 1)     */
    uint32_t mip_levels;         /* >= 1                                        */
    uint32_t sample_count;       /* 1 only in v1                                */
    uint32_t explicit_row_pitch; /* bytes; 0 = natural (64-byte aligned rows)   */
    uint64_t plane_offset_B;     /* offset of level 0 inside the resource       */
} mali_texture_params_t;

/* Create a texture description and compute its layout. *data_size_B is the
 * resource size it needs (plane_offset_B included). NULL if the layout is
 * unsupported (e.g. explicit pitch with mips, pitch not 64-byte aligned). */
mali_texture_t *mali_texture_new(const mali_ctx_t *ctx, const mali_texture_params_t *p,
                                 uint64_t *data_size_B);
void            mali_texture_set_base(mali_texture_t *t, uint64_t resource_gpu_va);
/* View over (levels, slices) of `parent` with a possibly different format and
 * type; only reinterpretations of equal pixel size are accepted. */
mali_texture_t *mali_texture_view(const mali_texture_t *parent, uint32_t mtl_pixel_format,
                                  uint32_t mtl_texture_type, uint32_t first_level,
                                  uint32_t level_count, uint32_t first_slice, uint32_t slice_count);
void            mali_texture_free(mali_texture_t *t);

typedef struct mali_subresource {
    uint64_t offset_B;           /* from byte 0 of the resource                 */
    uint32_t row_stride_B;
    uint64_t slice_stride_B;     /* between z-slices (3D) or array layers (via array stride) */
    uint32_t width, height, depth;
    uint64_t size_B;
} mali_subresource_t;
bool mali_texture_subresource(const mali_texture_t *t, uint32_t level, uint32_t slice,
                              mali_subresource_t *out);
uint32_t        mali_texture_pipe_format(const mali_texture_t *t);
uint32_t        mali_texture_bpp(const mali_texture_t *t);
uint64_t        mali_texture_base(const mali_texture_t *t);

/* ===================================================================== *
 * 5. Descriptors  (mali_desc.c). All 32-byte descriptors are written to a
 *    caller buffer of 8 words; payloads (texture surface arrays) come from an
 *    arena and are never freed individually.
 * ===================================================================== */
typedef struct mali_sampler_params {     /* distilled MTLSamplerDescriptor      */
    uint32_t min_filter, mag_filter;     /* MTLSamplerMinMagFilter: 0 nearest, 1 linear */
    uint32_t mip_filter;                 /* MTLSamplerMipFilter: 0 none, 1 nearest, 2 linear */
    uint32_t addr_s, addr_t, addr_r;     /* MTLSamplerAddressMode               */
    uint32_t border;                     /* MTLSamplerBorderColor 0..2          */
    uint32_t max_anisotropy;             /* 1..16                               */
    uint32_t compare;                    /* MTLCompareFunction (0 never .. 7 always); 8 = none */
    bool     normalized_coords;
    float    lod_min, lod_max;
    float    lod_bias;
} mali_sampler_params_t;
void mali_emit_sampler(const mali_sampler_params_t *p, bool integer_texture, uint32_t desc[8]);

bool mali_emit_sampled_texture(const mali_texture_t *t, mali_arena_t *payload_arena, uint32_t desc[8]);
bool mali_emit_storage_texture(const mali_texture_t *t, mali_arena_t *payload_arena, uint32_t desc[8]);
void mali_emit_buffer_desc(uint64_t gpu, uint64_t size, uint32_t desc[8]);
/* Tier-A FS varying: an ATTRIBUTE into the VS vertex packet at vs_offset [M:vulkan/csf/panvk_vX_cmd_draw.c
 * emit_varying_descs]. False when (alu_type, ncomps) has no varying format. */
bool mali_emit_varying_desc(uint8_t alu_type, uint8_t ncomps, uint32_t vs_offset, uint32_t generic_size_B,
                            uint32_t desc[8]);
void mali_emit_null_desc(uint32_t desc[8]);
void mali_emit_resource_entry(uint64_t table_gpu, uint64_t size_B, bool contains_desc, uint32_t out[4]);

/* ===================================================================== *
 * 6. Shaders and pipelines
 * ===================================================================== */
typedef struct mali_shader {
    air2vh_program prog;         /* copy of the compiler's flat program struct  */
    uint64_t code_gpu;           /* start of the uploaded binary (128-aligned)  */
    uint64_t spd_gpu[3];         /* shader program descriptors [S07 8.2]        */
    uint32_t vary_size_B;        /* VS: generic varying size (RUN_IDVS VARY_SIZE register) */
    bool     valid;
} mali_shader_t;

/* generic_size_B of a VS's layout blob (struct pan_varying_layout), 0 if absent. */
uint32_t mali_vs_vary_size(const air2vh_vs_layout *l);

#define MALI_SPD_MEM_BYTES        (3u * 32u)
/* Write the SPDs for `s` into spd_mem (96 bytes, 32-aligned). Needs code_gpu. */
bool mali_shader_build_spds(mali_shader_t *s, mali_gpu_ptr_t spd_mem);
/* True if [code_gpu, code_gpu+code_size) lies inside one 4 GiB window (the
 * inline constant pool is PC-relative with a 32-bit offset) [S07 6.13]. */
bool mali_code_window_ok(uint64_t code_gpu, uint32_t code_size);

/* Metal MTLVertexDescriptor distilled. buffer indices are Metal buffer slots. */
typedef struct mali_vertex_attr { uint32_t mtl_format; uint32_t offset; uint32_t buffer_index; } mali_vertex_attr_t;
typedef struct mali_vertex_layout {
    uint32_t stride;             /* bytes                                       */
    uint32_t step_function;      /* MTLVertexStepFunction: 0 const, 1 per-vertex, 2 per-instance */
    uint32_t step_rate;
} mali_vertex_layout_t;

typedef struct mali_rt_state {   /* one color attachment of a pipeline          */
    uint32_t mtl_format;         /* 0 = MTLPixelFormatInvalid (unused RT)       */
    bool     blend_enable;
    uint32_t src_rgb, dst_rgb, op_rgb;    /* MTLBlendFactor / MTLBlendOperation */
    uint32_t src_alpha, dst_alpha, op_alpha;
    uint32_t write_mask;         /* MTLColorWriteMask: R=8 G=4 B=2 A=1          */
} mali_rt_state_t;

/* True when `rt`'s blend equation cannot be done by the fixed-function blender and the
 * pipeline therefore needs a blend shader [M:vulkan/panvk_vX_blend.c blend_needs_shader]. */
bool   mali_blend_rt_needs_shader(const mali_rt_state_t *rt, const float constants[4]);
/* Key bytes (a struct pan_blend_shader_key) for air2vh_compile_blend_shader(); returns the key
 * size or 0. src0_nir_type is the FS output's nir_alu_type (air2vh_program.fs.rt[i].base_type). */
size_t mali_blend_shader_key(const mali_rt_state_t *rt, unsigned rt_index, uint8_t src0_nir_type,
                             void *out_key, size_t cap);

typedef struct mali_gfx_pipeline {
    mali_shader_t vs, fs;        /* fs.valid false = no fragment function (depth-only) */
    uint32_t rt_count;           /* highest used RT + 1                         */
    mali_rt_state_t rt[MALI_MAX_RTS];
    uint32_t depth_format, stencil_format;   /* MTLPixelFormat or 0              */
    uint32_t sample_count;
    bool     alpha_to_coverage, alpha_to_one, rasterization_enabled;
    uint32_t vattr_mask;         /* attributes the pipeline defines             */
    mali_vertex_attr_t   vattr[MALI_MAX_VATTRS];
    mali_vertex_layout_t vlayout[MALI_MAX_BUFFERS];
    uint32_t input_prim_class;   /* MTLPrimitiveTopologyClass: 0 unspec 1 point 2 line 3 triangle */
    uint64_t blend_shader_gpu[MALI_MAX_RTS]; /* 0 = fixed function; else blend shader address */
    uint32_t fs_vary_count;      /* tier-A FS: T0 varying descriptors, 0 = none */
    uint32_t fs_vary_desc[AIR2VH_EXT_MAX_VARYINGS][8];
} mali_gfx_pipeline_t;

typedef struct mali_compute_pipeline {
    mali_shader_t cs;
    uint32_t tg_threads_hint;    /* max total threads per threadgroup the shader allows */
} mali_compute_pipeline_t;

/* ===================================================================== *
 * 7. Resource bindings of one stage (snapshot taken at draw/dispatch time)
 * ===================================================================== */
typedef struct mali_stage_binds {
    uint64_t serial;                          /* bumped by the encoder on ANY change below;
                                               * equal serial + same program lets a pass reuse the SRT */
    uint64_t buf_gpu[MALI_MAX_BUFFERS];       /* GPU VA incl. offset; 0 = unbound  */
    uint64_t buf_size[MALI_MAX_BUFFERS];      /* bytes from buf_gpu to resource end */
    uint32_t tex_desc[MALI_MAX_TEXTURES][8];  /* prebuilt sampled descriptors      */
    uint32_t tex_mask;
    uint32_t stex_desc[MALI_MAX_TEXTURES][8]; /* prebuilt storage descriptors      */
    uint32_t stex_mask;
    uint32_t samp_desc[MALI_MAX_SAMPLERS + 1][8]; /* [16] = dummy sampler (always valid) */
    uint32_t samp_mask;
    uint32_t tg_mem_len[MALI_MAX_BUFFERS];    /* threadgroup(n) lengths, compute only */
} mali_stage_binds_t;

/* Build T0..T4 and the 8-entry SRT for one program; returns the SRT register
 * value (srt_gpu | 8) or 0 on allocation failure. `t0_extra` is the T0 table
 * (vertex attributes/vertex buffers) the caller prepared, or NULL. */
uint64_t mali_build_srt(mali_arena_t *a, const air2vh_program *prog, const mali_stage_binds_t *b,
                        const mali_gpu_ptr_t *t0, uint32_t t0_count);
/* FAU: fill from the sysval struct per prog->fau; returns FAU register value
 * (gpu | slots<<56) or 0 when the program has no FAU. */
uint64_t mali_build_fau(mali_arena_t *a, const air2vh_program *prog,
                        const void *sysvals, size_t sysvals_size);

/* ===================================================================== *
 * 8. Command stream builder  (mali_cs.c)
 * ===================================================================== */
typedef struct mali_cs mali_cs_t;            /* opaque: wraps struct cs_builder       */

mali_cs_t *mali_cs_new(const mali_ctx_t *ctx, mali_arena_t *cs_arena);
void       mali_cs_free(mali_cs_t *cs);
/* Finish the stream: returns false if any chunk allocation failed. root_va is
 * 64-byte aligned and size a multiple of 8 [MALI_CS_STREAM_ALIGN]. */
bool       mali_cs_end(mali_cs_t *cs, uint64_t *root_va, uint32_t *size_B);
bool       mali_cs_is_empty(const mali_cs_t *cs);

/* There is no explicit init call: the first compute dispatch, render pass or barrier of a
 * stream emits its prologue (REQ_RESOURCE, scoreboard state) and the first render pass
 * its HEAP_SET (from the queue-state buffer in mali_pass_desc.heap_desc_gpu). Every stream
 * is therefore self-contained and may run first or after any other. */

/* ---- compute ---- */
typedef struct mali_dispatch {
    const mali_compute_pipeline_t *pipe;
    uint32_t grid[3];            /* threadgroups                                */
    uint32_t tg_size[3];         /* threads per threadgroup                     */
    uint32_t threads_per_grid[3];/* dispatchThreads size, else grid*tg_size     */
    mali_stage_binds_t *binds;
    bool     nonuniform;         /* program was compiled with the guard         */
    uint64_t scratch_gpu;        /* TLS backing (tls_size * instances), may be 0 */
    uint64_t wls_gpu;            /* WLS backing                                  */
    mali_arena_t *desc_arena;    /* per-command-buffer descriptor arena          */
} mali_dispatch_t;
bool mali_cs_dispatch(mali_cs_t *cs, const mali_ctx_t *ctx, const mali_dispatch_t *d);

/* ---- explicit scoreboard drain + cache flush between encoders (S06 3.5) ---- */
void mali_cs_encoder_barrier(mali_cs_t *cs);

/* ---- render pass ---- */
enum { MALI_LOAD_DONTCARE = 0, MALI_LOAD_LOAD = 1, MALI_LOAD_CLEAR = 2 };      /* MTLLoadAction  */
enum { MALI_STORE_DONTCARE = 0, MALI_STORE_STORE = 1 };                        /* MTLStoreAction */

typedef struct mali_attachment {
    const mali_texture_t *tex;   /* NULL = unused                                */
    uint32_t level, slice;
    uint32_t load, store;
    float    clear_color[4];
    double   clear_depth;
    uint32_t clear_stencil;
} mali_attachment_t;

typedef struct mali_pass_desc {
    uint32_t width, height;      /* render target size                           */
    uint32_t sample_count;       /* 1                                            */
    uint32_t layers;             /* 1                                            */
    mali_attachment_t color[MALI_MAX_RTS];
    uint32_t color_count;        /* highest used + 1                             */
    mali_attachment_t depth, stencil;
    uint64_t heap_desc_gpu;      /* TILER_HEAP descriptor VA (queue state buffer) */
    mali_arena_t *desc_arena;
} mali_pass_desc_t;

typedef struct mali_pass mali_pass_t;
mali_pass_t *mali_pass_begin(mali_cs_t *cs, const mali_ctx_t *ctx, const mali_pass_desc_t *d);

typedef struct mali_stencil_face {
    uint32_t compare;            /* MTLCompareFunction                          */
    uint32_t fail_op, depth_fail_op, pass_op;   /* MTLStencilOperation          */
    uint32_t read_mask, write_mask;
} mali_stencil_face_t;
typedef struct mali_zs_state {   /* distilled MTLDepthStencilDescriptor          */
    uint32_t depth_compare;      /* MTLCompareFunction (7 = Always)              */
    bool     depth_write;
    bool     stencil_enable;     /* any non-default stencil operation            */
    mali_stencil_face_t front, back;
} mali_zs_state_t;

/* Encoder state at draw time (everything dynamic in Metal). */
typedef struct mali_draw_state {
    const mali_gfx_pipeline_t *pipe;
    mali_stage_binds_t *vs_binds, *fs_binds;
    /* fixed-function state */
    struct { float x, y, w, h, znear, zfar; } viewport;     /* MTLViewport (originY down) */
    bool     viewport_set;
    struct { uint32_t x, y, w, h; } scissor;
    bool     scissor_set;
    uint32_t cull_mode;          /* MTLCullMode 0 none 1 front 2 back              */
    uint32_t winding;            /* MTLWinding 0 clockwise 1 counter-clockwise     */
    uint32_t fill_mode;          /* MTLTriangleFillMode 0 fill 1 lines             */
    uint32_t depth_clip_mode;    /* MTLDepthClipMode 0 clip 1 clamp                */
    float    depth_bias, depth_slope, depth_clamp;
    float    blend_color[4];
    uint32_t stencil_ref_front, stencil_ref_back;
    const mali_zs_state_t *zs;   /* NULL = depth/stencil state not set (always pass, no write) */
    uint32_t visibility_mode;    /* MTLVisibilityResultMode (0 disabled)           */
    /* per-draw */
    uint32_t prim;               /* MTLPrimitiveType                               */
    bool     indexed;
    uint32_t index_type;         /* MTLIndexType 0 uint16, 1 uint32                */
    uint64_t index_gpu;          /* address of first index to read                 */
    uint32_t index_buffer_size;  /* bytes from index_gpu to end of buffer          */
    uint32_t count;              /* vertex count or index count                    */
    uint32_t instance_count;
    int32_t  base_vertex;
    uint32_t vertex_start;       /* non-indexed first vertex                       */
    uint32_t base_instance;
    mali_arena_t *desc_arena;
    uint64_t tls_gpu; uint32_t tls_bytes;  /* TLS backing sized for this pass     */
    mali_gpu_ptr_t t0_vs;        /* T0 table (vertex attributes + vbuf descs), or {0} */
    uint32_t t0_vs_count;
} mali_draw_state_t;

bool mali_pass_draw(mali_pass_t *p, const mali_draw_state_t *s);
/* LoadActionLoad is not handled here: the Objective-C encoder issues a fullscreen draw as the
 * first draw of the pass (MaliRenderEncoder.m, shaders/MaliInternal.metal). */
bool mali_pass_end(mali_pass_t *p);          /* emits FINISH_TILING .. FINISH_FRAGMENT */
void mali_pass_abort(mali_pass_t *p);        /* free without emitting (encoder failed) */

/* Tiler heap queue-state layout (the buffer the plug-in allocates per queue). */
#define MALI_QSTATE_HEAP_DESC_OFF   0x0000u  /* TILER_HEAP descriptor, written by the kernel */
#define MALI_QSTATE_RESULT_OFF      0x0080u  /* struct mali_heap_result, written by the kernel */
#define MALI_QSTATE_GEOM_OFF        0x1000u  /* 64 KiB geometry buffer [MV:panvk_vX_gpu_queue.c:455-459] */
#define MALI_QSTATE_GEOM_SIZE       0x10000u
#define MALI_QSTATE_SIZE            (MALI_QSTATE_GEOM_OFF + MALI_QSTATE_GEOM_SIZE)

#ifdef __cplusplus
}
#endif
#endif /* MALI_CORE_H */
