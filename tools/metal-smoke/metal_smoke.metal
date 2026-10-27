#include <metal_stdlib>
using namespace metal;

kernel void mul_add(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                    uint gid [[thread_position_in_grid]])
{
    out[gid] = in[gid] * 3u + 7u;
}
