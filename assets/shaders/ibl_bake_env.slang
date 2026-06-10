// ibl_bake_env.slang — bake the sky into an environment cubemap (Phase 7.6;
// atmosphere LUT path added in 11.1). One thread per cube texel; writes the
// sky radiance for that texel's world direction. sun_dir.w selects the model:
// 0 = procedural sky (no LUTs bound), 1 = physically-based sky-view LUT with a
// transmittance-tinted sun disc, so reflections and ambient match the
// background. The result feeds the irradiance + prefilter convolutions.

import atmosphere;
import ibl_common;
import sky;

// Must match SKY_LUT_BRIGHTNESS in lighting.slang.
static const float SKY_LUT_BRIGHTNESS = 8.0;

struct BakeParams {
    float4 sun_dir;  // xyz = direction to the sun, w = 1 to use the LUT sky
    uint   face_size; // pixels per cube face edge
    uint   pad0;
    uint   pad1;
    uint   pad2;
}
[[vk::push_constant]] BakeParams pc;

[[vk::binding(0)]] RWTexture2DArray<float4> g_out; // 6-layer cube as a 2D array
[[vk::binding(1)]] Texture2D    g_skyview;         // atmosphere LUTs (may be fallbacks)
[[vk::binding(2)]] Texture2D    g_transmittance;
[[vk::binding(3)]] SamplerState g_samp;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.face_size || tid.y >= pc.face_size) {
        return;
    }
    float2 uv = (float2(tid.xy) + 0.5) / float(pc.face_size);
    float3 dir = cube_dir(tid.z, uv);
    float3 sun = normalize(pc.sun_dir.xyz);

    float3 radiance;
    if (pc.sun_dir.w > 0.5) {
        radiance = sample_skyview(g_skyview, g_samp, dir, sun) * SKY_LUT_BRIGHTNESS;
        float cos_sun = dot(dir, sun);
        if (cos_sun > 0.9998) {
            float3 sun_trans =
                sample_transmittance(g_transmittance, g_samp, ATMOS_VIEW_HEIGHT, sun.y);
            radiance += sun_trans * 80.0 * smoothstep(0.9998, 0.99995, cos_sun);
        }
    } else {
        radiance = sky(dir, sun);
    }
    g_out[tid] = float4(radiance, 1.0);
}
