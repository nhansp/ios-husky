// AIR intrinsic coverage for air2valhall: compile with Apple's metal for iOS, then
//   mali-shadercache -o /tmp/t.mvpack --metallib intrinsics.metallib --disasm
#include <metal_stdlib>
using namespace metal;

kernel void math_f32(device float4 *o [[buffer(0)]], device const float4 *a [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    float4 x = a[i], y = a[i + 1];
    o[i * 16 + 0] = round(x);
    o[i * 16 + 1] = tan(x);
    o[i * 16 + 2] = asin(x);
    o[i * 16 + 3] = acos(x);
    o[i * 16 + 4] = atan(x);
    o[i * 16 + 5] = sinh(x);
    o[i * 16 + 6] = cosh(x);
    o[i * 16 + 7] = tanh(x);
    o[i * 16 + 8] = exp10(x);
    o[i * 16 + 9] = log10(x);
    o[i * 16 + 10] = fmod(x, y);
    o[i * 16 + 11] = ldexp(x, int4(y));
    o[i * 16 + 12] = median3(x, y, x * y);
    o[i * 16 + 13] = precise::round(x) + fast::round(y);
    o[i * 16 + 14] = fast::tan(x) + fast::exp10(y);
    o[i * 16 + 15] = fma(x, y, x);
}

kernel void math_f16(device half4 *o [[buffer(0)]], device const half4 *a [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    half4 x = a[i], y = a[i + 1];
    o[i * 6 + 0] = round(x);
    o[i * 6 + 1] = tanh(x);
    o[i * 6 + 2] = sinh(x) + cosh(y);
    o[i * 6 + 3] = fmod(x, y);
    o[i * 6 + 4] = asin(x) + acos(y) + atan(x);
    o[i * 6 + 5] = log10(x) + exp10(y);
}

kernel void bits_u32(device uint4 *o [[buffer(0)]], device const uint4 *a [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    uint4 x = a[i], y = a[i + 1];
    o[i * 10 + 0] = popcount(x);
    o[i * 10 + 1] = clz(x);
    o[i * 10 + 2] = ctz(x);
    o[i * 10 + 3] = reverse_bits(x);
    o[i * 10 + 4] = rotate(x, y);
    o[i * 10 + 5] = mulhi(x, y);
    o[i * 10 + 6] = addsat(x, y);
    o[i * 10 + 7] = subsat(x, y);
    o[i * 10 + 8] = extract_bits(x, 3u, 7u);
    o[i * 10 + 9] = insert_bits(x, y, 5u, 9u) + median3(x, y, x ^ y);
}

kernel void bits_i32(device int4 *o [[buffer(0)]], device const int4 *a [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    int4 x = a[i], y = a[i + 1];
    o[i * 5 + 0] = mulhi(x, y);
    o[i * 5 + 1] = addsat(x, y);
    o[i * 5 + 2] = subsat(x, y);
    o[i * 5 + 3] = extract_bits(x, 2u, 11u);
    o[i * 5 + 4] = median3(x, y, x - y) + popcount(x) + clz(y);
}

kernel void bits_u16(device ushort4 *o [[buffer(0)]], device const ushort4 *a [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    ushort4 x = a[i], y = a[i + 1];
    o[i * 4 + 0] = popcount(x) + clz(x) + ctz(y);
    o[i * 4 + 1] = reverse_bits(x);
    o[i * 4 + 2] = rotate(x, y);
    o[i * 4 + 3] = extract_bits(x, 1u, 5u) + insert_bits(x, y, 2u, 6u) + addsat(x, y);
}
