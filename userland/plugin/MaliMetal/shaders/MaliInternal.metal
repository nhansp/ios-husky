/*
 * MaliInternal.metal - the plug-in's own shaders
 * -------------------------------------------------------------------------
 * Intended repo path: plugin/MaliMetal/shaders/MaliInternal.metal
 *
 * Built to AIR at build time (build.sh: xcrun metal -c), embedded in the bundle's __MALI,__internal section and
 * compiled by air2valhall on first use (MaliInternal.m). They exist so the command builder can stay ordered inside
 * one command stream (MaliPluginABI.h D-1, D-4):
 *   - mali_fullscreen_vs / mali_load_*   LoadActionLoad: the FBD preload path would need NIR frame shaders, so the
 *                                        encoder draws the attachment's old contents with these (MaliRenderEncoder.m);
 *   - mali_fill_*, mali_copy2d_*         blit-encoder fills and (linear) copies; textures are linear in v1, so a
 *                                        texture<->buffer or texture<->texture copy is a strided memory copy;
 *   - mali_mip_box_f                     generateMipmapsForTexture: for float-class formats.
 * All kernels guard their own bounds, so they are dispatched with rounded-up threadgroup counts.
 */
#include <metal_stdlib>
using namespace metal;

/* ---------------- LoadActionLoad ---------------- */
struct MaliFSQ { float4 position [[position]]; };

vertex MaliFSQ mali_fullscreen_vs(uint vid [[vertex_id]]) {
    const float2 p = float2((vid << 1) & 2, vid & 2);       /* (0,0) (2,0) (0,2): one triangle covering the target */
    MaliFSQ o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

#define MALI_LOAD_FS(N, SUF, OUTTYPE, TEXTYPE)                                                              \
    struct MaliLoadOut_##SUF##N { OUTTYPE c [[color(N)]]; };                                                \
    fragment MaliLoadOut_##SUF##N mali_load_##SUF##N(MaliFSQ in [[stage_in]],                               \
                                                     texture2d<TEXTYPE, access::read> src [[texture(0)]]) { \
        MaliLoadOut_##SUF##N o;                                                                             \
        o.c = src.read(uint2(in.position.xy));                                                              \
        return o;                                                                                           \
    }
#define MALI_LOAD_ALL(N)                                                                                    \
    MALI_LOAD_FS(N, f, float4, float)                                                                       \
    MALI_LOAD_FS(N, u, uint4, uint)                                                                         \
    MALI_LOAD_FS(N, i, int4, int)
MALI_LOAD_ALL(0) MALI_LOAD_ALL(1) MALI_LOAD_ALL(2) MALI_LOAD_ALL(3)
MALI_LOAD_ALL(4) MALI_LOAD_ALL(5) MALI_LOAD_ALL(6) MALI_LOAD_ALL(7)

/* ---------------- fills ---------------- */
struct MaliFillParams { uint count; uint value; };

kernel void mali_fill_u32(device uint *dst [[buffer(0)]], constant MaliFillParams &p [[buffer(1)]],
                          uint gid [[thread_position_in_grid]]) {
    if (gid < p.count) dst[gid] = p.value;
}
kernel void mali_fill_u8(device uchar *dst [[buffer(0)]], constant MaliFillParams &p [[buffer(1)]],
                         uint gid [[thread_position_in_grid]]) {
    if (gid < p.count) dst[gid] = (uchar)p.value;
}

/* ---------------- strided copies: every field is in elements (bytes for _u8, words for _u32) ---------------- */
struct MaliCopyParams {
    uint row_units;              /* elements per row                                 */
    uint rows;
    uint depth;
    uint src_row, src_img;       /* element strides                                  */
    uint dst_row, dst_img;
};

kernel void mali_copy2d_u8(device const uchar *src [[buffer(0)]], device uchar *dst [[buffer(1)]],
                           constant MaliCopyParams &p [[buffer(2)]], uint3 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.row_units || gid.y >= p.rows || gid.z >= p.depth) return;
    dst[gid.z * p.dst_img + gid.y * p.dst_row + gid.x] = src[gid.z * p.src_img + gid.y * p.src_row + gid.x];
}
kernel void mali_copy2d_u32(device const uint *src [[buffer(0)]], device uint *dst [[buffer(1)]],
                            constant MaliCopyParams &p [[buffer(2)]], uint3 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.row_units || gid.y >= p.rows || gid.z >= p.depth) return;
    dst[gid.z * p.dst_img + gid.y * p.dst_row + gid.x] = src[gid.z * p.src_img + gid.y * p.src_row + gid.x];
}

/* ---------------- mip generation (2x2 box, float-class formats) ---------------- */
kernel void mali_mip_box_f(texture2d<float, access::read> src [[texture(0)]],
                           texture2d<float, access::write> dst [[texture(1)]],
                           uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    const uint2 hi = uint2(src.get_width() - 1, src.get_height() - 1);
    const uint2 s = gid * 2;
    const float4 a = src.read(min(s, hi));
    const float4 b = src.read(min(s + uint2(1, 0), hi));
    const float4 c = src.read(min(s + uint2(0, 1), hi));
    const float4 d = src.read(min(s + uint2(1, 1), hi));
    dst.write((a + b + c + d) * 0.25, gid);
}
