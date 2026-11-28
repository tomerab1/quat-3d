// cloud_march.slang — the volumetric cloud march (ROADMAP R13.4,
// VolumetricCloudPass), at quarter resolution (half width x half height).
//
// Each texel marches its view ray through the cloud shell of lib/clouds.slang
// from where it enters to where it leaves (or the farthest opaque surface of
// its 2x2 full-resolution block), with a per-frame jittered start so the
// temporal pass (cloud_temporal.slang) accumulates many offsets into a
// converged image — Schneider 2015 / Hillaire 2016's quarter-resolution
// march plus temporal reprojection. Empty space is skipped with the cheap
// (base-only) density; inside a cloud each sample marches toward the sun and
// the moon, sums Wrenninge's scattering octaves plus the sky's ambient and
// integrates the step energy-conservingly. Behind the cumulus layer the
// cirrus slab is added, then the aerial perspective (Hillaire 2020) between
// the camera and the clouds' transmittance-weighted depth.
//
// Output: g_cloud rgb = luminance (per unit extra-atmospheric solar
// irradiance, the sky-view LUT unit), a = transmittance; g_cloud_depth = the
// transmittance-weighted cloud distance in km (CLOUD_NO_DEPTH when clear).

import atmosphere;
import aerial_perspective;
import cloud_noise;
import clouds;

static const float CLOUD_NO_DEPTH = 1.0e4;

// Per-frame march values (std140; engine::renderer::CloudMarchGpu).
struct CloudMarch {
    float4 inv_view_proj[4]; // clip -> world (render jitter removed), glm columns
    float4 camera;           // xyz world position (m), w unused
    float4 extent;           // xy cloud extent, zw full render extent
    float4 frame;            // x frame index, y base step count, zw unused
}
ConstantBuffer<CloudLayer> g_layer;
ConstantBuffer<CloudMarch> g_march;

Texture3D    g_base;
Texture3D    g_detail;
Texture2D    g_curl;
Texture2D    g_weather;
SamplerState g_noise_sampler; // trilinear, repeat
Texture2D    g_transmittance;
Texture2D    g_skyview;
Texture3D    g_ap;
SamplerState g_lut_sampler; // bilinear, clamp
Texture2D    g_depth;       // full-resolution scene depth
[format("rgba16f")] RWTexture2D<float4> g_cloud;
[format("r32f")] RWTexture2D<float>     g_cloud_depth;

float4 mul_cols4(float4 cols[4], float4 v) {
    return cols[0] * v.x + cols[1] * v.y + cols[2] * v.z + cols[3] * v.w;
}

float3 world_at(float2 uv, float depth) {
    float4 h = mul_cols4(g_march.inv_view_proj, float4(uv * 2.0 - 1.0, depth, 1.0));
    return h.xyz / h.w;
}

// Distance (km) along the ray to the farthest opaque surface of this
// texel's 2x2 full-resolution block (infinite for the sky).
float scene_limit_km(uint2 texel, float3 dir) {
    uint2 full = uint2(g_march.extent.zw);
    float far_depth = 0.0;
    bool sky = false;
    for (uint i = 0; i < 4; ++i) {
        uint2 c = min(texel * 2u + uint2(i & 1u, i >> 1u), full - 1u);
        float d = g_depth.Load(int3(int2(c), 0)).r;
        if (d >= 1.0) sky = true;
        far_depth = max(far_depth, d);
    }
    if (sky) return 1.0e9;
    float2 uv = (float2(texel) * 2.0 + 1.0) / float2(full);
    float3 p = world_at(uv, far_depth);
    // Distance along this texel's ray to the depth of that surface.
    float3 to_p = p - g_march.camera.xyz;
    return length(to_p) * 0.001 / max(dot(normalize(to_p), dir), 0.1);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint2 size = uint2(g_march.extent.xy);
    if (tid.x >= size.x || tid.y >= size.y) return;
    int2 coord = int2(tid.xy);
    float2 uv = (float2(tid.xy) + 0.5) / float2(size);
    float3 near_p = world_at(uv, 0.0);
    float3 far_p = world_at(uv, 1.0);
    float3 dir = normalize(far_p - near_p);
    CloudLayer L = g_layer;
    float3 ambient_top = cloud_sky_ambient(g_skyview, g_lut_sampler, L.sun.xyz);

    // Per-frame jitter: interleaved gradient noise shifted along a
    // low-discrepancy (golden ratio) sequence.
    float frame = g_march.frame.x;
    float jitter = frac(interleaved_gradient_noise(float2(tid.xy) + 5.588238 * fmod(frame, 64.0)) +
                        0.6180340 * fmod(frame, 256.0));

    float limit = scene_limit_km(tid.xy, dir);
    CloudRay ray = march_cloud_layer(L, g_base, g_detail, g_curl, g_weather, g_noise_sampler,
                                     g_transmittance, g_lut_sampler, dir, limit, jitter,
                                     ambient_top, g_march.frame.y);
    float r = ATMOS_VIEW_HEIGHT;
    float mu = dir.y;
    float3 lum = ray.lum;
    float trans = ray.trans;
    float depth_sum = ray.depth_sum;
    float weight_sum = ray.weight_sum;
    // Cirrus behind the cumulus layer (never in front of an opaque surface
    // nearer than the layer).
    if (limit > 1.0e8 && trans > 0.005) {
        float4 ci = cirrus_layer(L, g_weather, g_curl, g_noise_sampler, g_transmittance,
                                 g_lut_sampler, dir, ambient_top);
        if (ci.a < 1.0) {
            float t_c = cloud_layer_distance(r, mu, ATMOS_GROUND_RADIUS + L.cirrus.y);
            float w = trans * (1.0 - ci.a);
            lum += trans * ci.rgb;
            trans *= ci.a;
            depth_sum += t_c * w;
            weight_sum += w;
        }
    }
    float depth = weight_sum > 1e-4 ? depth_sum / weight_sum : CLOUD_NO_DEPTH;
    if (weight_sum > 1e-4) {
        // Aerial perspective in front of the clouds (Hillaire 2020): their
        // light is attenuated by the air and the air's own in-scattering
        // replaces the sky they hide.
        float4 ap = sample_aerial_perspective(g_ap, g_lut_sampler, uv, depth);
        lum = lum * ap.a + ap.rgb * (1.0 - trans);
    }
    g_cloud[coord] = float4(lum, trans);
    g_cloud_depth[coord] = depth;
}
