/*
 * mali_internal.h - declarations shared between the core's translation units
 * (not visible to the Objective-C side).
 * Intended repo path: plugin/MaliMetal/core/mali_internal.h
 */
#ifndef MALI_INTERNAL_H
#define MALI_INTERNAL_H

#include "mali_core.h"

struct pan_image_view;

const struct pan_image_view *mali_texture_pan_view(const mali_texture_t *t);
uint32_t mali_texture_mtl_format(const mali_texture_t *t);
uint32_t mali_texture_flags(const mali_texture_t *t);
void     mali_texture_dims(const mali_texture_t *t, uint32_t *w, uint32_t *h, uint32_t *layers);

/* GL/Valhall helpers used by more than one file */
#define MALI_ALIGN(v, a) (((v) + (a) - 1) & ~((uint64_t)(a) - 1))

static inline uint32_t mali_f2u(float f) { union { float f; uint32_t u; } x = {.f = f}; return x.u; }


/* mali_state.c */
struct pan_earlyzs_state;
bool mali_state_emit_blend(mali_arena_t *a, const mali_gfx_pipeline_t *pipe, const float constants[4],
                           uint64_t *bds_reg, uint64_t blend_descs_out[MALI_MAX_RTS], bool *any_dest_read);
void mali_state_dcd(const mali_gfx_pipeline_t *pipe, const mali_draw_state_t *s, bool any_dest_read,
                    uint32_t out[3], struct pan_earlyzs_state *ez);
void mali_state_prim_flags(const mali_gfx_pipeline_t *pipe, const mali_draw_state_t *s, uint32_t *flags,
                           uint32_t *flags2, uint32_t *override);
bool mali_state_emit_zsd(mali_arena_t *a, const mali_gfx_pipeline_t *pipe, const mali_draw_state_t *s,
                         uint64_t *zsd_gpu);
bool mali_state_emit_tiler_ctx(mali_arena_t *a, const mali_ctx_t *ctx, uint64_t heap_desc_gpu, uint32_t width,
                               uint32_t height, uint32_t tile_size_px, uint64_t *gpu);

#endif
