/*
 * SPDX-License-Identifier: MIT
 *
 * vh_sampler.c - AIR constexpr sampler word -> Mali Valhall SAMPLER descriptor.
 * -------------------------------------------------------------------------
 * Intended repo path: userland/air2valhall/src/vh_sampler.c
 *
 * Derivative work adapting air2ir3/src/a8xx_sampler.c (MIT / Mesa freedreno
 * attribution): air_sampler_decode, half_bits_to_float, the compare table,
 * wrap mapping, and JSON formatting are adapted from air2ir3.
 *   AIR word layout: proven by air2ir3 against Metal on an M3 Max
 *     (A2/src/a8xx_sampler.c:12-21 notes, decode at :66-75):
 *       bits 0-2 s_address, 3-5 t_address, 6-8 r_address, 9 mag_linear,
 *       11 min_linear, 13-14 mip_filter, 15 pixel_coords, 16-19 compare (MSL
 *       order), 20-23 aniso-1, 24-39 lod_min (half), 40-55 lod_max (half),
 *       56-57 border.
 *   Mali field positions: v11.xml:1393-1421 (Sampler struct), enums
 *     Func :77-86, Mipmap Mode :408-413, LOD Algorithm :415-419,
 *     Wrap Mode :461-468, Descriptor Type :42-51.
 *   Field policy (spec 07 6.6, all [D] unless cited): mirrors
 *     panvk_sampler_fill_desc (MP/vulkan/panvk_vX_sampler.c:103-180).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "vh_sampler.h"

static float half_bits_to_float(uint16_t h)
{
    unsigned sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, man = h & 0x3ffu;
    float v;
    if (exp == 0)
        v = ldexpf((float)man, -24);                 /* subnormal */
    else if (exp == 31)
        v = man ? NAN : INFINITY;
    else
        v = ldexpf((float)(man | 0x400u), (int)exp - 25);
    return sign ? -v : v;
}

void air_sampler_decode(uint64_t w, struct air_sampler *s)
{
    memset(s, 0, sizeof *s);
    s->word = w;
    s->s_address = (unsigned)(w & 7u);
    s->t_address = (unsigned)((w >> 3) & 7u);
    s->r_address = (unsigned)((w >> 6) & 7u);
    s->mag_linear = (unsigned)((w >> 9) & 1u);
    s->min_linear = (unsigned)((w >> 11) & 1u);
    s->mip_filter = (unsigned)((w >> 13) & 3u);
    s->pixel_coords = (unsigned)((w >> 15) & 1u);
    s->compare = (unsigned)((w >> 16) & 0xfu);
    s->aniso = (unsigned)((w >> 20) & 0xfu) + 1u;
    s->lod_min = half_bits_to_float((uint16_t)(w >> 24));
    s->lod_max = half_bits_to_float((uint16_t)(w >> 40));
    s->border = (unsigned)((w >> 56) & 3u);
}

void air_sampler_a8xx(const struct air_sampler *s, uint32_t d[4])
{
    (void)s;
    d[0] = d[1] = d[2] = d[3] = 0;   /* VH-ABI 1 does not use the Adreno descriptor */
}

/* Mali "Func" numbering (v11.xml:77-86). */
enum { MF_NEVER = 0, MF_LESS = 1, MF_EQUAL = 2, MF_LEQUAL = 3, MF_GREATER = 4,
       MF_NOTEQUAL = 5, MF_GEQUAL = 6, MF_ALWAYS = 7 };

unsigned air_sampler_compare_to_mali(unsigned c)
{
    /* Step 1, AIR (MSL order, A2/src/a8xx_sampler.c:15-21) -> GL/Vulkan
     * semantics, the mapping air2ir3 proved for all 9 values against Metal
     * (a8xx_sampler.c:46-49): 1 less, 2 lequal, 3 greater, 4 gequal, 5 equal,
     * 6 notequal, 7 always, 0/8 never.
     * Step 2, GL/Vulkan -> Mali: swap LESS<->GREATER, LEQUAL<->GEQUAL
     * (pan_flip_compare_func, MP/lib/pan_encoder.h:157-175, applied the same
     * way by panvk, MP/vulkan/panvk_vX_sampler.c:54-61).
     * [U] composed from two sources; confirm with a sample_compare probe (R5). */
    static const unsigned map[9] = {
        MF_NEVER,                 /* 0: never emitted, treated as 8 */
        MF_GREATER,               /* 1 less          -> LESS    -> flip */
        MF_GEQUAL,                /* 2 less_equal    -> LEQUAL  -> flip */
        MF_LESS,                  /* 3 greater       -> GREATER -> flip */
        MF_LEQUAL,                /* 4 greater_equal -> GEQUAL  -> flip */
        MF_EQUAL,                 /* 5 equal                            */
        MF_NOTEQUAL,              /* 6 not_equal                        */
        MF_ALWAYS,                /* 7 always                           */
        MF_NEVER,                 /* 8 never / none                     */
    };
    return c < 9 ? map[c] : 0xffffffffu;
}

/* AIR address mode -> Mali Wrap Mode (v11.xml:461-468); unknown codes ->
 * clamp to edge, as air2ir3 does (A2/src/a8xx_sampler.c:55-64). */
static unsigned wrap_of(unsigned a)
{
    switch (a) {
    case 0: return 11;   /* clamp_to_zero / clamp_to_border -> Clamp to Border */
    case 1: return 9;    /* clamp_to_edge          -> Clamp to Edge            */
    case 2: return 8;    /* repeat                 -> Repeat                   */
    case 3: return 12;   /* mirrored_repeat        -> Mirrored Repeat          */
    case 4: return 13;   /* mirror_clamp_to_edge   -> Mirrored Clamp to Edge   */
    default: return 9;
    }
}

/* 13-bit unsigned LOD, 8 fractional bits, clamped
 * (genxml "ulod": util_bitpack_ufixed_clamp(v, ., ., 8), gen_pack.py:355;
 *  range [0, 8191/256], spec 07 6.6). NaN -> 0. */
static uint32_t ulod13(float v)
{
    const float max = 8191.0f / 256.0f;
    if (!(v > 0.0f))
        return 0;
    if (v > max)
        v = max;
    return (uint32_t)lroundf(v * 256.0f) & 0x1fffu;
}

static void set_bits(uint32_t *w, unsigned start, unsigned size, uint32_t v)
{
    uint32_t m = (size >= 32) ? 0xffffffffu : ((1u << size) - 1u);
    w[start >> 5] = (w[start >> 5] & ~(m << (start & 31))) | ((v & m) << (start & 31));
}

void air_sampler_valhall(const struct air_sampler *s, uint32_t d[8], uint32_t int_border[4])
{
    memset(d, 0, 32);
    const int aniso = s->aniso > 1;
    const int both_nearest = !s->mag_linear && !s->min_linear;

    /* Mipmap Mode (v11.xml:408-413): none -> None(1), nearest -> Nearest(0),
     * linear -> Trilinear(3), or Performance Trilinear(2) with anisotropy
     * (panvk does the same, panvk_vX_sampler.c:20-33,115-116).
     * [U] whether None returns level 0 exactly as Metal (R5). */
    unsigned mip = s->mip_filter == 0 ? 1u : s->mip_filter == 1 ? 0u : (aniso ? 2u : 3u);

    unsigned wrap_s = wrap_of(s->s_address), wrap_t = wrap_of(s->t_address);
    unsigned wrap_r = s->pixel_coords ? 9u : wrap_of(s->r_address);  /* unnormalised: R ignored
                                                                         (panvk_vX_sampler.c:157-160) */
    /* word 0 */
    set_bits(d, 0, 4, 1);                          /* Type Sampler (:1394, :44)            */
    set_bits(d, 4, 2, 0);                          /* Reduction mode Average (:1395)       */
    set_bits(d, 8, 4, wrap_r);                     /* Wrap Mode R @0:8  (:1396)            */
    set_bits(d, 12, 4, wrap_t);                    /* Wrap Mode T @0:12 (:1397)            */
    set_bits(d, 16, 4, wrap_s);                    /* Wrap Mode S @0:16 (:1398)            */
    set_bits(d, 21, 1, both_nearest ? 0u : 1u);    /* Round to nearest even (:1399); off for
                                                      NEAREST/NEAREST (panvk_vX_sampler.c:134-137) */
    set_bits(d, 22, 1, 0);                         /* sRGB override (:1401)                */
    set_bits(d, 23, 1, 1);                         /* Seamless Cube Map (:1402)            */
    set_bits(d, 24, 1, 0);                         /* Clamp integer coordinates (:1403)    */
    set_bits(d, 25, 1, s->pixel_coords ? 0u : 1u); /* Normalized Coordinates (:1404)       */
    set_bits(d, 26, 1, 0);                         /* Clamp integer array indices = false
                                                      (panvk_vX_sampler.c:118)             */
    set_bits(d, 27, 1, s->min_linear ? 0u : 1u);   /* Minify nearest (:1405)               */
    set_bits(d, 28, 1, s->mag_linear ? 0u : 1u);   /* Magnify nearest (:1406)              */
    set_bits(d, 29, 1, 0);                         /* Magnify cutoff (:1408)               */
    set_bits(d, 30, 2, mip);                       /* Mipmap Mode @0:30 (:1409)            */

    /* word 1: LOD clamps + compare. Unnormalised coordinates sample level 0
     * only (A2/src/a8xx_sampler.c:91; Metal pixel coordinates). */
    float lo = s->pixel_coords ? 0.0f : s->lod_min;
    float hi = s->pixel_coords ? 0.0f : s->lod_max;
    unsigned cmp = air_sampler_compare_to_mali(s->compare);
    if (cmp == 0xffffffffu)
        cmp = MF_NEVER;
    set_bits(d, 32 + 0, 13, ulod13(lo));           /* Minimum LOD @1:0  (:1411)            */
    set_bits(d, 32 + 13, 3, cmp);                  /* Compare Function @1:13 (:1412)       */
    set_bits(d, 32 + 16, 13, ulod13(hi));          /* Maximum LOD @1:16 (:1413)            */

    /* word 2: LOD bias 0 (Metal constexpr samplers have none), anisotropy. */
    set_bits(d, 64 + 0, 16, 0);                    /* LOD bias (:1414)                     */
    set_bits(d, 64 + 16, 5, aniso ? (s->aniso - 1u) : 0u); /* Maximum anisotropy, minus(1) (:1415) */
    set_bits(d, 64 + 24, 2, aniso ? 3u : 0u);      /* LOD algorithm Anisotropic/Isotropic (:1416;
                                                      panvk_vX_sampler.c:171-174)          */

    /* words 4..7: border colour stored in the descriptor (:1417-1420).
     * AIR border 0 transparent_black, 1 opaque_black, 2 opaque_white
     * (A2/src/a8xx_sampler.c:53); 3 is not emitted by Metal -> transparent. */
    const uint32_t one_f = 0x3f800000u;
    uint32_t fb[4] = {0, 0, 0, 0}, ib[4] = {0, 0, 0, 0};
    if (s->border == 1) {
        fb[3] = one_f; ib[3] = 1;
    } else if (s->border == 2) {
        fb[0] = fb[1] = fb[2] = fb[3] = one_f;
        ib[0] = ib[1] = ib[2] = ib[3] = 1;          /* [U] Metal's integer border value */
    }
    memcpy(&d[4], fb, sizeof fb);
    if (int_border)
        memcpy(int_border, ib, sizeof ib);
}

static const char *const k_addr[8] = {"clamp_to_zero_or_border", "clamp_to_edge", "repeat",
                                      "mirrored_repeat", "mirror_clamp_to_edge", "addr5",
                                      "addr6", "addr7"};
static const char *const k_cmp[9] = {"none_or_never", "less", "less_equal", "greater",
                                     "greater_equal", "equal", "not_equal", "always",
                                     "none_or_never"};
static const char *const k_border[4] = {"transparent_black", "opaque_black", "opaque_white",
                                        "border3"};

/* JSON has no inf/nan literals; AIR's default lod_max is +inf (half 0x7c00). */
static void json_float(char *b, size_t n, float v)
{
    if (isnan(v))
        snprintf(b, n, "\"nan\"");
    else if (isinf(v))
        snprintf(b, n, v < 0 ? "\"-inf\"" : "\"inf\"");
    else
        snprintf(b, n, "%g", (double)v);
}

int air_sampler_json(const struct air_sampler *s, char *out, size_t n)
{
    char lo[24], hi[24];
    json_float(lo, sizeof lo, s->lod_min);
    json_float(hi, sizeof hi, s->lod_max);
    return snprintf(out, n,
        "{\"s_address\":\"%s\",\"t_address\":\"%s\",\"r_address\":\"%s\","
        "\"mag_filter\":\"%s\",\"min_filter\":\"%s\",\"mip_filter\":\"%s\","
        "\"coord\":\"%s\",\"compare_func\":\"%s\",\"air_compare\":%u,"
        "\"mali_compare_func\":%u,\"max_anisotropy\":%u,"
        "\"lod_min_clamp\":%s,\"lod_max_clamp\":%s,\"border_color\":\"%s\"}",
        k_addr[s->s_address & 7], k_addr[s->t_address & 7], k_addr[s->r_address & 7],
        s->mag_linear ? "linear" : "nearest", s->min_linear ? "linear" : "nearest",
        s->mip_filter == 0 ? "none" : s->mip_filter == 1 ? "nearest" : "linear",
        s->pixel_coords ? "pixel" : "normalized",
        s->compare < 9 ? k_cmp[s->compare] : "?", s->compare,
        s->compare < 9 ? air_sampler_compare_to_mali(s->compare) : 99u,
        s->aniso, lo, hi, k_border[s->border & 3]);
}
