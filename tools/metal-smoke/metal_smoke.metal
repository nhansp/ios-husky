#include <metal_stdlib>
using namespace metal;

kernel void mul_add(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                    uint gid [[thread_position_in_grid]])
{
    out[gid] = in[gid] * 3u + 7u;
}

struct FlatOut {
    float4 position [[position]];
};

vertex FlatOut flat_vs(uint vid [[vertex_id]], const device float2 *pos [[buffer(0)]])
{
    FlatOut o;
    o.position = float4(pos[vid], 0.0, 1.0);
    return o;
}

fragment float4 flat_fs()
{
    return float4(1.0, 0.0, 1.0, 1.0);
}

struct ColorOut {
    float4 position [[position]];
    float4 color;
};

vertex ColorOut color_vs(uint vid [[vertex_id]], const device float2 *pos [[buffer(0)]],
                         const device float4 *color [[buffer(1)]])
{
    ColorOut o;
    o.position = float4(pos[vid], 0.0, 1.0);
    o.color = color[vid];
    return o;
}

fragment float4 color_fs(ColorOut in [[stage_in]])
{
    return in.color;
}
