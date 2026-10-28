/*
 * mali_texture.c - texture layout and views on top of Mesa's pan_image
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/core/mali_texture.c
 *
 * A Metal texture becomes one `struct pan_image` (linear modifier, one plane)
 * plus a `struct pan_image_view`. Mesa's pan_image_layout_init is the single
 * source of the level offsets and strides: the CPU copy paths (replaceRegion,
 * getBytes) and the GPU descriptors (pan_sampled_texture_emit,
 * pan_emit_linear_color_attachment) read the same numbers [M:lib/pan_layout.c:
 * pan_image_layout_init; M:lib/pan_image.h struct pan_image].
 *
 * IOSurface-backed textures pass the surface's bytes-per-row as an explicit
 * layout constraint (wsi_row_pitch_B), exactly what panvk does for imported
 * linear images [M:lib/pan_layout.c "use_explicit_layout"]; Mesa rejects
 * pitches below its 64-byte linear row alignment, in which case this returns
 * NULL and the Objective-C side builds an aligned shadow (MaliResources.m).
 */
#include "mali_mesa.h"   /* first: see the note there */
#include "mali_core.h"
#include "mali_internal.h"

#include <stdlib.h>
#include <string.h>

struct mali_image_shared {
    int                    refs;
    struct pan_image       image;
    struct pan_image_plane plane;
    uint32_t               mtl_format;
    uint32_t               bpp;
    uint32_t               flags;
    uint64_t               plane_offset_B;
};

struct mali_texture {
    struct mali_image_shared *img;
    struct pan_image_view     view;
    uint32_t                  mtl_format;     /* of the view */
    uint32_t                  width, height, depth;
    uint32_t                  layers;         /* array layers in the view (cube: faces) */
    uint32_t                  levels;
    uint32_t                  first_level, first_layer;
};

static enum mali_texture_dimension
view_dim(uint32_t mtl_type)
{
    switch (mtl_type) {
    case MALI_TEX_1D:
    case MALI_TEX_1D_ARRAY:
        return MALI_TEXTURE_DIMENSION_1D;
    case MALI_TEX_CUBE:
    case MALI_TEX_CUBE_ARRAY:
        return MALI_TEXTURE_DIMENSION_CUBE;
    case MALI_TEX_3D:
        return MALI_TEXTURE_DIMENSION_3D;
    default:
        return MALI_TEXTURE_DIMENSION_2D;
    }
}

static void
fill_view(struct mali_texture *t, const struct pan_image_view *proto_unused)
{
    (void)proto_unused;
}

mali_texture_t *
mali_texture_new(const mali_ctx_t *ctx, const mali_texture_params_t *p, uint64_t *data_size_B)
{
    mali_fmt_info_t fi;
    if (!p || !mali_fmt_lookup(p->mtl_pixel_format, &fi) || !(fi.flags & MALI_FMT_SAMPLED || fi.flags & MALI_FMT_COLOR_RT ||
                                                               fi.flags & (MALI_FMT_DEPTH | MALI_FMT_STENCIL)))
        return NULL;
    const uint32_t samples = p->sample_count ? p->sample_count : 1;
    if ((samples > 1) != (p->mtl_texture_type == MALI_TEX_2D_MS) || p->mtl_texture_type == MALI_TEX_2D_MS_ARRAY ||
        p->mtl_texture_type == MALI_TEX_BUFFER || (samples > 1 && (p->mip_levels != 1 || p->explicit_row_pitch)))
        return NULL;
    if (!p->width || !p->height || !p->mip_levels || p->mip_levels > MAX_MIP_LEVELS)
        return NULL;

    const bool cube = p->mtl_texture_type == MALI_TEX_CUBE || p->mtl_texture_type == MALI_TEX_CUBE_ARRAY;
    const bool is3d = p->mtl_texture_type == MALI_TEX_3D;
    const uint32_t array_length = p->array_length ? p->array_length : 1;
    const uint32_t layers = cube ? array_length * 6 : array_length;
    const uint32_t depth = is3d ? (p->depth ? p->depth : 1) : 1;

    struct mali_image_shared *img = calloc(1, sizeof(*img));
    struct mali_texture *t = calloc(1, sizeof(*t));
    if (!img || !t) {
        free(img);
        free(t);
        return NULL;
    }

    img->refs = 1;
    img->mtl_format = p->mtl_pixel_format;
    img->bpp = fi.bytes_per_pixel;
    img->flags = fi.flags;
    img->plane_offset_B = p->plane_offset_B;
    img->image.props = (struct pan_image_props){
        .modifier = DRM_FORMAT_MOD_LINEAR,
        .format = (enum pipe_format)fi.pipe_format,
        .extent_px = {.width = p->width, .height = p->height, .depth = depth},
        .nr_samples = samples,
        .dim = is3d ? MALI_TEXTURE_DIMENSION_3D : (p->mtl_texture_type <= MALI_TEX_1D_ARRAY ? MALI_TEXTURE_DIMENSION_1D
                                                                                            : MALI_TEXTURE_DIMENSION_2D),
        .nr_slices = p->mip_levels,
        .array_size = layers,
        .crc = false,
    };
    img->image.mod_handler = pan_mod_get_handler(MALI_ARCH, DRM_FORMAT_MOD_LINEAR);
    img->image.planes[0] = &img->plane;

    struct pan_image_layout_constraints cons = {
        .offset_B = p->plane_offset_B,
        .wsi_row_pitch_B = p->explicit_row_pitch,
        /* array textures with an explicit pitch are not produced by any IOSurface
         * path in v1 */
    };
    if (!img->image.mod_handler || !pan_image_layout_init(MALI_ARCH, &img->image, 0, &cons)) {
        free(img);
        free(t);
        return NULL;
    }

    t->img = img;
    t->mtl_format = p->mtl_pixel_format;
    t->width = p->width;
    t->height = p->height;
    t->depth = depth;
    t->layers = layers;
    t->levels = p->mip_levels;
    t->first_level = 0;
    t->first_layer = 0;
    t->view = (struct pan_image_view){
        .format = (enum pipe_format)fi.pipe_format,
        .dim = view_dim(p->mtl_texture_type),
        .first_level = 0,
        .last_level = p->mip_levels - 1,
        .first_layer_or_z_slice = 0,
        .last_layer_or_z_slice = is3d ? depth - 1 : layers - 1,
        .min_lod = 0.0f,
        .swizzle = {PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W},
        .planes = {{.image = &img->image, .plane_idx = 0}},
        .nr_samples = samples,
    };
    /* A single-slice non-array 2D texture is viewed as a plain 2D texture; a
     * 3D texture one slice deep is also fine [spec 05: CA binds 1x1x1 ones]. */

    if (data_size_B)
        *data_size_B = p->plane_offset_B + img->plane.layout.data_size_B;
    (void)ctx;
    (void)fill_view;
    return t;
}

void
mali_texture_set_base(mali_texture_t *t, uint64_t gpu)
{
    t->img->plane.base = gpu;
}

uint64_t
mali_texture_base(const mali_texture_t *t)
{
    return t->img->plane.base;
}

mali_texture_t *
mali_texture_view(const mali_texture_t *parent, uint32_t mtl_pixel_format, uint32_t mtl_texture_type,
                  uint32_t first_level, uint32_t level_count, uint32_t first_slice, uint32_t slice_count)
{
    mali_fmt_info_t fi;
    if (!mali_fmt_lookup(mtl_pixel_format, &fi))
        return NULL;
    /* View-based pixel re-interpretation requires equal block size [M:lib/pan_image.h
     * pan_image_view_check]. */
    if (fi.bytes_per_pixel != parent->img->bpp)
        return NULL;
    if (first_level + level_count > parent->levels || first_slice + slice_count > parent->layers || !level_count ||
        !slice_count)
        return NULL;

    struct mali_texture *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->img = parent->img;
    t->img->refs++;
    t->mtl_format = mtl_pixel_format;
    t->first_level = parent->first_level + first_level;
    t->first_layer = parent->first_layer + first_slice;
    t->levels = level_count;
    t->layers = slice_count;
    t->width = u_minify(parent->width, first_level);
    t->height = u_minify(parent->height, first_level);
    t->depth = mtl_texture_type == MALI_TEX_3D ? u_minify(parent->depth, first_level) : 1;

    const bool is3d = view_dim(mtl_texture_type) == MALI_TEXTURE_DIMENSION_3D;
    t->view = parent->view;
    t->view.format = (enum pipe_format)fi.pipe_format;
    t->view.dim = view_dim(mtl_texture_type);
    t->view.first_level = t->first_level;
    t->view.last_level = t->first_level + level_count - 1;
    t->view.first_layer_or_z_slice = is3d ? 0 : t->first_layer;
    t->view.last_layer_or_z_slice = is3d ? t->depth - 1 : t->first_layer + slice_count - 1;
    t->view.planes[0].image = &t->img->image;
    return t;
}

void
mali_texture_set_swizzle(mali_texture_t *t, const uint8_t mtl_swizzle[4])
{
    static const unsigned char map[6] = {PIPE_SWIZZLE_0, PIPE_SWIZZLE_1, PIPE_SWIZZLE_X,
                                         PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W};
    for (unsigned i = 0; i < 4; i++)
        t->view.swizzle[i] = mtl_swizzle[i] < 6 ? map[mtl_swizzle[i]] : (unsigned char)(PIPE_SWIZZLE_X + i);
}

void
mali_texture_free(mali_texture_t *t)
{
    if (!t)
        return;
    if (--t->img->refs == 0)
        free(t->img);
    free(t);
}

bool
mali_texture_subresource(const mali_texture_t *t, uint32_t level, uint32_t slice, mali_subresource_t *out)
{
    const struct mali_image_shared *img = t->img;
    const uint32_t L = t->first_level + level;
    const uint32_t S = t->first_layer + slice;
    if (level >= t->levels || L >= img->image.props.nr_slices)
        return false;
    const bool is3d = img->image.props.dim == MALI_TEXTURE_DIMENSION_3D;
    if (!is3d && S >= img->image.props.array_size)
        return false;
    const struct pan_image_slice_layout *sl = &img->plane.layout.slices[L];
    out->offset_B = sl->offset_B + (is3d ? 0 : (uint64_t)S * img->plane.layout.array_stride_B);
    out->row_stride_B = sl->tiled_or_linear.row_stride_B;
    out->slice_stride_B = sl->tiled_or_linear.surface_stride_B;
    out->width = u_minify(img->image.props.extent_px.width, L);
    out->height = u_minify(img->image.props.extent_px.height, L);
    out->depth = is3d ? u_minify(img->image.props.extent_px.depth, L) : 1;
    out->size_B = sl->size_B;
    return true;
}

uint32_t
mali_texture_pipe_format(const mali_texture_t *t)
{
    return (uint32_t)t->view.format;
}

uint32_t
mali_texture_bpp(const mali_texture_t *t)
{
    return t->img->bpp;
}

/* Used by mali_desc.c / mali_pass.c (mali_internal.h). */
const struct pan_image_view *
mali_texture_pan_view(const mali_texture_t *t)
{
    return &t->view;
}

uint32_t
mali_texture_mtl_format(const mali_texture_t *t)
{
    return t->mtl_format;
}

uint32_t
mali_texture_flags(const mali_texture_t *t)
{
    return t->img->flags;
}

void
mali_texture_dims(const mali_texture_t *t, uint32_t *w, uint32_t *h, uint32_t *layers)
{
    *w = t->width;
    *h = t->height;
    *layers = t->layers;
}
