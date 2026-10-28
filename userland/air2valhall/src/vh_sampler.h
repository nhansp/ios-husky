/*
 * vh_sampler.h - AIR constexpr-sampler decode + Mali Valhall SAMPLER packing.
 * -------------------------------------------------------------------------
 * Intended repo path: userland/air2valhall/src/vh_sampler.h
 *
 * Replaces air2ir3's a8xx_sampler.h in the air2valhall translation unit
 * (spec 07 3.1 "a8xx_sampler.c/.h: split", 3.3 item 2).
 *
 * Mesa-free C. ORIGINAL implementation; the AIR word layout is the one
 * air2ir3 proved against Metal (A2/src/a8xx_sampler.c:12-21, 66-75).
 */
#ifndef VH_SAMPLER_H
#define VH_SAMPLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct air_sampler {
    uint64_t word;
    unsigned s_address, t_address, r_address, mag_linear, min_linear, mip_filter,
             pixel_coords, compare, aniso, border;
    float lod_min, lod_max;
};

void air_sampler_decode(uint64_t w, struct air_sampler *s);
/* compatibility stub: writes zeros */
void air_sampler_a8xx(const struct air_sampler *s, uint32_t d[4]);
int  air_sampler_json(const struct air_sampler *s, char *out, size_t n);

/*
 * Mali SAMPLER (v11.xml:1393-1421) for an AIR sampler state, spec 07 6.6.
 *   d[8]          float-texture descriptor (border words = float bits)
 *   int_border[4] border words for integer textures (replace d[4..7])
 */
void air_sampler_valhall(const struct air_sampler *s, uint32_t d[8], uint32_t int_border[4]);

/* AIR (MSL order) compare value -> Mali "Func" (v11.xml:77-86), after the
 * GL->Mali LESS/GREATER flip (MP/lib/pan_encoder.h:157-175). 0xffffffff for
 * an invalid value. */
unsigned air_sampler_compare_to_mali(unsigned air_compare);

#ifdef __cplusplus
}
#endif
#endif /* VH_SAMPLER_H */
