/*
 * mali_formats.c - MTLPixelFormat / MTLVertexFormat -> Mesa pipe_format
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/core/mali_formats.c
 *
 * The v1 plug-in answers the formats CoreAnimation can ask for [spec 05
 * sections 4.1-4.3, 6 rows 12/15]: BGRA8, RGBA8, R8, A8, RG8 first, then the
 * 16-bit/float/10-bit families. Extended-range XR formats, YCbCr, block
 * compressed formats and packed 24-bit depth answer false: MaliMTLDevice
 * reports supportsExtendedXR10Formats NO [spec 05 section 7] and the device
 * never advertises the rest.
 *
 * MTLPixelFormat numeric values are the Metal SDK enum values (stable ABI).
 * Each row notes the Mali capability class from M:lib/pan_format.c semantics:
 * every colour format listed here is renderable and blendable on Valhall
 * except the 32-bit-per-channel ones (rendered, not blended) [S07 6.10].
 */
#include "mali_core.h"

#include "util/format/u_formats.h"

#define F_RT_BLEND (MALI_FMT_COLOR_RT | MALI_FMT_SAMPLED | MALI_FMT_BLENDABLE | MALI_FMT_STORAGE | MALI_FMT_CPU_LINEAR)
#define F_RT_NOBL  (MALI_FMT_COLOR_RT | MALI_FMT_SAMPLED | MALI_FMT_STORAGE | MALI_FMT_CPU_LINEAR)
#define F_TEX_ONLY (MALI_FMT_SAMPLED | MALI_FMT_CPU_LINEAR)

struct fmt_row {
    uint16_t mtl;
    uint8_t  bpp;
    uint32_t flags;
    int      pipe;
};

/* Values: MTLPixelFormat* in <Metal/MTLPixelFormat.h>. */
static const struct fmt_row fmt_table[] = {
    { 1,   1, F_TEX_ONLY | MALI_FMT_COLOR_RT, PIPE_FORMAT_A8_UNORM },            /* A8Unorm            */
    { 10,  1, F_RT_BLEND,                     PIPE_FORMAT_R8_UNORM },             /* R8Unorm            */
    { 11,  1, F_RT_BLEND | MALI_FMT_SRGB,     PIPE_FORMAT_R8_SRGB },              /* R8Unorm_sRGB       */
    { 12,  1, F_RT_BLEND,                     PIPE_FORMAT_R8_SNORM },             /* R8Snorm            */
    { 13,  1, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R8_UINT },              /* R8Uint             */
    { 14,  1, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R8_SINT },              /* R8Sint             */
    { 20,  2, F_RT_BLEND,                     PIPE_FORMAT_R16_UNORM },            /* R16Unorm           */
    { 22,  2, F_RT_BLEND,                     PIPE_FORMAT_R16_SNORM },            /* R16Snorm           */
    { 23,  2, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R16_UINT },             /* R16Uint            */
    { 24,  2, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R16_SINT },             /* R16Sint            */
    { 25,  2, F_RT_BLEND | MALI_FMT_FLOAT,    PIPE_FORMAT_R16_FLOAT },            /* R16Float           */
    { 30,  2, F_RT_BLEND,                     PIPE_FORMAT_R8G8_UNORM },           /* RG8Unorm           */
    { 31,  2, F_RT_BLEND | MALI_FMT_SRGB,     PIPE_FORMAT_R8G8_SRGB },            /* RG8Unorm_sRGB      */
    { 32,  2, F_RT_BLEND,                     PIPE_FORMAT_R8G8_SNORM },           /* RG8Snorm           */
    { 33,  2, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R8G8_UINT },            /* RG8Uint            */
    { 34,  2, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R8G8_SINT },            /* RG8Sint            */
    { 40,  2, F_RT_BLEND,                     PIPE_FORMAT_B5G6R5_UNORM },         /* B5G6R5Unorm        */
    { 41,  2, F_RT_BLEND,                     PIPE_FORMAT_A1B5G5R5_UNORM },       /* A1BGR5Unorm        */
    { 42,  2, F_RT_BLEND,                     PIPE_FORMAT_A4B4G4R4_UNORM },       /* ABGR4Unorm         */
    { 43,  2, F_RT_BLEND,                     PIPE_FORMAT_B5G5R5A1_UNORM },       /* BGR5A1Unorm        */
    { 53,  4, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R32_UINT },             /* R32Uint            */
    { 54,  4, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R32_SINT },             /* R32Sint            */
    { 55,  4, F_RT_NOBL | MALI_FMT_FLOAT,     PIPE_FORMAT_R32_FLOAT },            /* R32Float           */
    { 60,  4, F_RT_BLEND,                     PIPE_FORMAT_R16G16_UNORM },         /* RG16Unorm          */
    { 62,  4, F_RT_BLEND,                     PIPE_FORMAT_R16G16_SNORM },         /* RG16Snorm          */
    { 63,  4, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R16G16_UINT },          /* RG16Uint           */
    { 64,  4, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R16G16_SINT },          /* RG16Sint           */
    { 65,  4, F_RT_BLEND | MALI_FMT_FLOAT,    PIPE_FORMAT_R16G16_FLOAT },         /* RG16Float          */
    { 70,  4, F_RT_BLEND,                     PIPE_FORMAT_R8G8B8A8_UNORM },       /* RGBA8Unorm         */
    { 71,  4, F_RT_BLEND | MALI_FMT_SRGB,     PIPE_FORMAT_R8G8B8A8_SRGB },        /* RGBA8Unorm_sRGB    */
    { 72,  4, F_RT_BLEND,                     PIPE_FORMAT_R8G8B8A8_SNORM },       /* RGBA8Snorm         */
    { 73,  4, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R8G8B8A8_UINT },        /* RGBA8Uint          */
    { 74,  4, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R8G8B8A8_SINT },        /* RGBA8Sint          */
    { 80,  4, F_RT_BLEND,                     PIPE_FORMAT_B8G8R8A8_UNORM },       /* BGRA8Unorm         */
    { 81,  4, F_RT_BLEND | MALI_FMT_SRGB,     PIPE_FORMAT_B8G8R8A8_SRGB },        /* BGRA8Unorm_sRGB    */
    { 90,  4, F_RT_BLEND,                     PIPE_FORMAT_R10G10B10A2_UNORM },    /* RGB10A2Unorm       */
    { 91,  4, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R10G10B10A2_UINT },     /* RGB10A2Uint        */
    { 92,  4, F_RT_BLEND | MALI_FMT_FLOAT,    PIPE_FORMAT_R11G11B10_FLOAT },      /* RG11B10Float       */
    { 93,  4, F_TEX_ONLY | MALI_FMT_FLOAT,    PIPE_FORMAT_R9G9B9E5_FLOAT },       /* RGB9E5Float        */
    { 94,  4, F_RT_BLEND,                     PIPE_FORMAT_B10G10R10A2_UNORM },    /* BGR10A2Unorm       */
    { 103, 8, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R32G32_UINT },          /* RG32Uint           */
    { 104, 8, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R32G32_SINT },          /* RG32Sint           */
    { 105, 8, F_RT_NOBL | MALI_FMT_FLOAT,     PIPE_FORMAT_R32G32_FLOAT },         /* RG32Float          */
    { 110, 8, F_RT_BLEND,                     PIPE_FORMAT_R16G16B16A16_UNORM },   /* RGBA16Unorm        */
    { 112, 8, F_RT_BLEND,                     PIPE_FORMAT_R16G16B16A16_SNORM },   /* RGBA16Snorm        */
    { 113, 8, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R16G16B16A16_UINT },    /* RGBA16Uint         */
    { 114, 8, F_RT_NOBL | MALI_FMT_INTEGER,   PIPE_FORMAT_R16G16B16A16_SINT },    /* RGBA16Sint         */
    { 115, 8, F_RT_BLEND | MALI_FMT_FLOAT,    PIPE_FORMAT_R16G16B16A16_FLOAT },   /* RGBA16Float        */
    { 123, 16, F_RT_NOBL | MALI_FMT_INTEGER,  PIPE_FORMAT_R32G32B32A32_UINT },    /* RGBA32Uint         */
    { 124, 16, F_RT_NOBL | MALI_FMT_INTEGER,  PIPE_FORMAT_R32G32B32A32_SINT },    /* RGBA32Sint         */
    { 125, 16, F_RT_NOBL | MALI_FMT_FLOAT,    PIPE_FORMAT_R32G32B32A32_FLOAT },   /* RGBA32Float        */
    /* Depth/stencil: render attachments only; CPU copies refused (no linear pixel layout). */
    { 250, 2, MALI_FMT_DEPTH | MALI_FMT_SAMPLED,                      PIPE_FORMAT_Z16_UNORM },           /* Depth16Unorm           */
    { 252, 4, MALI_FMT_DEPTH | MALI_FMT_SAMPLED,                      PIPE_FORMAT_Z32_FLOAT },           /* Depth32Float           */
    { 253, 1, MALI_FMT_STENCIL,                                       PIPE_FORMAT_S8_UINT },             /* Stencil8               */
    { 260, 8, MALI_FMT_DEPTH | MALI_FMT_STENCIL | MALI_FMT_SAMPLED,  PIPE_FORMAT_Z32_FLOAT_S8X24_UINT },/* Depth32Float_Stencil8  */
    { 261, 8, MALI_FMT_STENCIL,                                       PIPE_FORMAT_X32_S8X24_UINT },      /* X32_Stencil8           */
};

bool
mali_fmt_lookup(uint32_t mtl, mali_fmt_info_t *out)
{
    for (unsigned i = 0; i < sizeof(fmt_table) / sizeof(fmt_table[0]); i++) {
        if (fmt_table[i].mtl != mtl)
            continue;
        out->pipe_format = fmt_table[i].pipe;
        out->bytes_per_pixel = fmt_table[i].bpp;
        out->flags = fmt_table[i].flags;
        return true;
    }
    out->pipe_format = PIPE_FORMAT_NONE;
    out->bytes_per_pixel = 0;
    out->flags = 0;
    return false;
}

/* MTLVertexFormat (Metal SDK values). Only formats Mali's attribute fetch
 * supports are mapped; the AIR front end normally loads vertex data itself
 * through the generated air.vertexFetchFunction (MaliPipeline.m), so this
 * table serves the hardware-attribute path (vs.attribute_mask != 0) only. */
int
mali_vertex_fmt_to_pipe(uint32_t f)
{
    switch (f) {
    case 1:  return PIPE_FORMAT_R8G8_UINT;            /* UChar2            */
    case 2:  return PIPE_FORMAT_R8G8B8_UINT;          /* UChar3            */
    case 3:  return PIPE_FORMAT_R8G8B8A8_UINT;        /* UChar4            */
    case 4:  return PIPE_FORMAT_R8G8_SINT;            /* Char2             */
    case 5:  return PIPE_FORMAT_R8G8B8_SINT;          /* Char3             */
    case 6:  return PIPE_FORMAT_R8G8B8A8_SINT;        /* Char4             */
    case 7:  return PIPE_FORMAT_R8G8_UNORM;           /* UChar2Normalized  */
    case 8:  return PIPE_FORMAT_R8G8B8_UNORM;         /* UChar3Normalized  */
    case 9:  return PIPE_FORMAT_R8G8B8A8_UNORM;       /* UChar4Normalized  */
    case 10: return PIPE_FORMAT_R8G8_SNORM;           /* Char2Normalized   */
    case 11: return PIPE_FORMAT_R8G8B8_SNORM;         /* Char3Normalized   */
    case 12: return PIPE_FORMAT_R8G8B8A8_SNORM;       /* Char4Normalized   */
    case 13: return PIPE_FORMAT_R16G16_UINT;          /* UShort2           */
    case 14: return PIPE_FORMAT_R16G16B16_UINT;       /* UShort3           */
    case 15: return PIPE_FORMAT_R16G16B16A16_UINT;    /* UShort4           */
    case 16: return PIPE_FORMAT_R16G16_SINT;          /* Short2            */
    case 17: return PIPE_FORMAT_R16G16B16_SINT;       /* Short3            */
    case 18: return PIPE_FORMAT_R16G16B16A16_SINT;    /* Short4            */
    case 19: return PIPE_FORMAT_R16G16_UNORM;         /* UShort2Normalized */
    case 20: return PIPE_FORMAT_R16G16B16_UNORM;      /* UShort3Normalized */
    case 21: return PIPE_FORMAT_R16G16B16A16_UNORM;   /* UShort4Normalized */
    case 22: return PIPE_FORMAT_R16G16_SNORM;         /* Short2Normalized  */
    case 23: return PIPE_FORMAT_R16G16B16_SNORM;      /* Short3Normalized  */
    case 24: return PIPE_FORMAT_R16G16B16A16_SNORM;   /* Short4Normalized  */
    case 25: return PIPE_FORMAT_R16G16_FLOAT;         /* Half2             */
    case 26: return PIPE_FORMAT_R16G16B16_FLOAT;      /* Half3             */
    case 27: return PIPE_FORMAT_R16G16B16A16_FLOAT;   /* Half4             */
    case 28: return PIPE_FORMAT_R32_FLOAT;            /* Float             */
    case 29: return PIPE_FORMAT_R32G32_FLOAT;         /* Float2            */
    case 30: return PIPE_FORMAT_R32G32B32_FLOAT;      /* Float3            */
    case 31: return PIPE_FORMAT_R32G32B32A32_FLOAT;   /* Float4            */
    case 32: return PIPE_FORMAT_R32_SINT;             /* Int               */
    case 33: return PIPE_FORMAT_R32G32_SINT;          /* Int2              */
    case 34: return PIPE_FORMAT_R32G32B32_SINT;       /* Int3              */
    case 35: return PIPE_FORMAT_R32G32B32A32_SINT;    /* Int4              */
    case 36: return PIPE_FORMAT_R32_UINT;             /* UInt              */
    case 37: return PIPE_FORMAT_R32G32_UINT;          /* UInt2             */
    case 38: return PIPE_FORMAT_R32G32B32_UINT;       /* UInt3             */
    case 39: return PIPE_FORMAT_R32G32B32A32_UINT;    /* UInt4             */
    case 40: return PIPE_FORMAT_R10G10B10A2_SNORM;    /* Int1010102Normalized  */
    case 41: return PIPE_FORMAT_R10G10B10A2_UNORM;    /* UInt1010102Normalized */
    case 42: return PIPE_FORMAT_B8G8R8A8_UNORM;       /* UChar4Normalized_BGRA */
    case 45: return PIPE_FORMAT_R8_UINT;              /* UChar             */
    case 46: return PIPE_FORMAT_R8_SINT;              /* Char              */
    case 47: return PIPE_FORMAT_R8_UNORM;             /* UCharNormalized   */
    case 48: return PIPE_FORMAT_R8_SNORM;             /* CharNormalized    */
    case 49: return PIPE_FORMAT_R16_UINT;             /* UShort            */
    case 50: return PIPE_FORMAT_R16_SINT;             /* Short             */
    case 51: return PIPE_FORMAT_R16_UNORM;            /* UShortNormalized  */
    case 52: return PIPE_FORMAT_R16_SNORM;            /* ShortNormalized   */
    case 53: return PIPE_FORMAT_R16_FLOAT;            /* Half              */
    case 54: return PIPE_FORMAT_R11G11B10_FLOAT;      /* FloatRG11B10      */
    case 55: return PIPE_FORMAT_R9G9B9E5_FLOAT;       /* FloatRGB9E5       */
    default: return 0;
    }
}
