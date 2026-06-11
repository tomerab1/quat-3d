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
    float4 cloud_a;  // coverage, extinction /km, bottom km, thickness km
    float4 cloud_b;  // 1/size km, detail erosion, unused, unused
    uint   face_size;   // pixels per cube face edge
    uint   face_offset; // first cube face this dispatch covers (dynamic IBL
                        // bakes per-face tiles; the full bake passes 0 and
                        // dispatches all 6 layers)
    uint   tile_x;      // texel offset of this dispatch's tile (0 = full face)
    uint   tile_y;
}
[[vk::push_constant]] BakeParams pc;

[[vk::binding(0)]] RWTexture2DArray<float4> g_out; // 6-layer cube as a 2D array
[[vk::binding(1)]] Texture2D    g_skyview;         // atmosphere LUTs (may be fallbacks)
[[vk::binding(2)]] Texture2D    g_transmittance;
[[vk::binding(3)]] SamplerState g_samp;

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
        // Atmosphere from the LUT + per-texel volumetric clouds (the same
        // march the lighting background uses), so reflections match the sky.
        float3 sky_rgb = sample_skyview(g_skyview, g_samp, dir, sun).rgb;
        float3 cloud_ambient =
            sample_skyview(g_skyview, g_samp, float3(0.2, 0.97, 0.0), sun).rgb * 0.8 +
            float3(0.008, 0.011, 0.016);
        CloudParams cp = unpack_cloud_params(pc.cloud_a, pc.cloud_b);
        float jitter = interleaved_gradient_noise(
            float2(px.x + face * pc.face_size, px.y));
        float4 clouds =
            march_clouds(dir, sun, g_transmittance, g_samp, cloud_ambient, cp, jitter);
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
