// ibl_bake_env.slang — bake the sky into an environment cubemap (Phase 7.6;
// atmosphere LUT path added in 11.1). One thread per cube texel; writes the
// sky radiance for that texel's world direction. sun_dir.w selects the model:
// 0 = procedural sky (no LUTs bound), 1 = physically-based sky-view LUT with a
// transmittance-tinted sun disc, so reflections and ambient match the
// background. The result feeds the irradiance + prefilter convolutions.
// R13.4: the LUT path bakes the volumetric clouds v2 (lib/clouds.slang, the
// layer and noise the cloud pass draws), so reflections and ambient light
// show the clouds of the sky.

import atmosphere;
import cloud_noise;
import clouds;
import ibl_common;
import sky;

// Must match SKY_LUT_BRIGHTNESS in lighting.slang.
static const float SKY_LUT_BRIGHTNESS = 8.0;

struct BakeParams {
    float4 sun_dir;  // xyz = direction to the sun, w = 1 to use the LUT sky
    float4 cloud_a;  // x = cloud march steps (0 = no clouds), yzw unused
    float4 cloud_b;  // unused
    uint   face_size;   // pixels per cube face edge
    uint   face_offset; // first cube face this dispatch covers (dynamic IBL
                        // bakes per-face tiles; the full bake passes 0 and
                        // dispatches all 6 layers)
    uint   tile_x;      // texel offset of this dispatch's tile (0 = full face)
    uint   tile_y;
}
[[vk::push_constant]] BakeParams pc;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2DArray<float4> g_out; // 6-layer cube as a 2D array
[[vk::binding(1)]] Texture2D    g_skyview;         // atmosphere LUTs (may be fallbacks)
[[vk::binding(2)]] Texture2D    g_transmittance;
[[vk::binding(3)]] SamplerState g_samp;
// R13.4 cloud layer + baked noise (fallbacks when cloud_a.x is 0).
[[vk::binding(4)]] ConstantBuffer<CloudLayer> g_layer;
[[vk::binding(5)]] Texture3D    g_base;
[[vk::binding(6)]] Texture3D    g_detail;
[[vk::binding(7)]] Texture2D    g_curl;
[[vk::binding(8)]] Texture2D    g_weather;
[[vk::binding(9)]] SamplerState g_noise_sampler;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint2 px = tid.xy + uint2(pc.tile_x, pc.tile_y);
    if (px.x >= pc.face_size || px.y >= pc.face_size) {
        return;
    }
    uint face = tid.z + pc.face_offset;
    float2 uv = (float2(px) + 0.5) / float(pc.face_size);
    float3 dir = cube_dir(face, uv);
    float3 sun = normalize(pc.sun_dir.xyz);

    float3 radiance;
    if (pc.sun_dir.w > 0.5) {
        // Atmosphere from the LUT + the volumetric clouds marched per texel
        // (no temporal accumulation here: the convolutions that follow
        // integrate the per-texel jitter away).
        float3 sky_rgb = sample_skyview(g_skyview, g_samp, dir, sun).rgb;
        float4 clouds = float4(0.0, 0.0, 0.0, 1.0);
        if (pc.cloud_a.x > 0.5) {
            CloudLayer L = g_layer;
            float3 ambient_top = cloud_sky_ambient(g_skyview, g_samp, sun);
            float jitter = interleaved_gradient_noise(float2(px.x + face * pc.face_size, px.y));
            CloudRay ray = march_cloud_layer(L, g_base, g_detail, g_curl, g_weather,
                                             g_noise_sampler, g_transmittance, g_samp, dir, 1.0e9,
                                             jitter, ambient_top, pc.cloud_a.x);
            float4 ci = cirrus_layer(L, g_weather, g_curl, g_noise_sampler, g_transmittance, g_samp,
                                     dir, ambient_top);
            float3 lum = ray.lum + ray.trans * ci.rgb;
            float trans = ray.trans * ci.a;
            // Aerial perspective toward the clouds, from the transmittance
            // LUT: the air between dims them and the sky's in-scattering
            // replaces what they hide.
            if (ray.weight_sum > 1e-4) {
                float d = ray.depth_sum / ray.weight_sum;
                float r = ATMOS_VIEW_HEIGHT;
                float rd = sqrt(r * r + d * d + 2.0 * r * d * dir.y);
                float mud = (r * dir.y + d) / rd;
                float3 t_ap = sample_transmittance(g_transmittance, g_samp, r, dir.y) /
                              max(sample_transmittance(g_transmittance, g_samp, rd, mud), 1e-4);
                float t = saturate(dot(t_ap, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)));
                lum = lum * t + sky_rgb * (1.0 - t) * (1.0 - trans);
            }
            clouds = float4(lum, trans);
        }
        radiance = (sky_rgb * clouds.a + clouds.rgb) * SKY_LUT_BRIGHTNESS;
        float cos_sun = dot(dir, sun);
        if (cos_sun > 0.9998) {
            float3 sun_trans =
                sample_transmittance(g_transmittance, g_samp, ATMOS_VIEW_HEIGHT, sun.y);
            radiance += sun_trans * clouds.a * 80.0 * smoothstep(0.9998, 0.99995, cos_sun);
        }
    } else {
        radiance = sky(dir, sun);
    }
    g_out[uint3(px, face)] = float4(radiance, 1.0);
}
