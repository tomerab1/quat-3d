// hdri_to_cube.slang — equirectangular HDRI -> environment cubemap mip 0
// (ROADMAP R21, HdriEnvironment).
//
// Each cube texel averages a 2x2 grid of bilinear taps of the panorama (wrap
// in u, clamp in v), read with Load from the RGBA32F source so values above
// half-float range survive until the final scale. The environment's rotation
// about +Y and its intensity are applied here, so every consumer (lighting,
// reflections, background) samples a plain world-space cube. With sun
// removal on, texels inside the sun's cone are clamped to the sky level
// around it: the analytic directional light (matched to the same sun,
// hdri_sun.cpp) carries that energy instead, with a shadow.

import ibl_common; // cube_dir
import equirect;
import hdr;        // hdr_store

struct HdriToCubeParams {
    float4 rotation;  // x cos, y sin of the rotation about +Y; z intensity; w face size
    float4 sun_cone;  // xyz sun direction (panorama space), w cos(cone radius); > 1 = keep
    float  sun_clamp; // luminance the sun's texels are clamped to
    uint   src_width;
    uint   src_height;
    uint   pad;
}
[[vk::push_constant]] HdriToCubeParams pc;

Texture2D<float4>                           g_src; // equirect RGBA32F
[format("rgba16f")] RWTexture2DArray<float4> g_out;

float3 load_texel(int x, int y) {
    int w = int(pc.src_width);
    int h = int(pc.src_height);
    x = ((x % w) + w) % w;
    y = clamp(y, 0, h - 1);
    float3 c = g_src.Load(int3(x, y, 0)).rgb;
    return max(c, float3(0.0, 0.0, 0.0));
}

float3 sample_panorama(float2 uv) {
    float2 p = uv * float2(float(pc.src_width), float(pc.src_height)) - 0.5;
    float2 f = p - floor(p);
    int2 i = int2(floor(p));
    float3 a = lerp(load_texel(i.x, i.y), load_texel(i.x + 1, i.y), f.x);
    float3 b = lerp(load_texel(i.x, i.y + 1), load_texel(i.x + 1, i.y + 1), f.x);
    return lerp(a, b, f.y);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint size = uint(pc.rotation.w);
    if (tid.x >= size || tid.y >= size) {
        return;
    }
    float3 sum = float3(0.0, 0.0, 0.0);
    for (uint s = 0; s < 4; ++s) {
        float2 offset = float2(float(s & 1u), float(s >> 1u)) * 0.5 + 0.25;
        float2 uv = (float2(tid.xy) + offset) / float(size);
        float3 d = world_to_panorama(cube_dir(tid.z, uv), pc.rotation.xy);
        float3 c = sample_panorama(equirect_uv(d));
        if (dot(d, pc.sun_cone.xyz) > pc.sun_cone.w) {
            float lum = eq_luminance(c);
            if (lum > pc.sun_clamp) {
                c *= pc.sun_clamp / lum;
            }
        }
        sum += c;
    }
    g_out[tid] = float4(hdr_store(sum * 0.25 * pc.rotation.z), 1.0);
}
