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
static const int   CLOUD_MAX_STEPS = 192; // iterations (coarse + fine) per ray
static const int   CLOUD_EMPTY_FINE = 6;  // empty fine samples before stepping coarse again
static const float CLOUD_MAX_MARCH_KM = 60.0;

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

float shell_distance(float r, float mu, float shell_radius) {
    float disc = r * r * (mu * mu - 1.0) + shell_radius * shell_radius;
    if (disc < 0.0) return -1.0;
    return -r * mu + sqrt(disc);
}

// The sky's mean radiance over the upper hemisphere: four sky-view samples
// (zenith, and 30 degrees up toward, away from and across the sun).
float3 sky_ambient(float3 sun) {
    float3 h = normalize(float3(sun.x, 0.0, sun.z) + float3(1e-4, 0.0, 0.0));
    float3 side = float3(-h.z, 0.0, h.x);
    const float c30 = 0.8660254;
    float3 a = sample_skyview(g_skyview, g_lut_sampler, float3(0.0, 1.0, 0.0), sun).rgb;
    a += sample_skyview(g_skyview, g_lut_sampler, h * c30 + float3(0.0, 0.5, 0.0), sun).rgb;
    a += sample_skyview(g_skyview, g_lut_sampler, -h * c30 + float3(0.0, 0.5, 0.0), sun).rgb;
    a += sample_skyview(g_skyview, g_lut_sampler, side * c30 + float3(0.0, 0.5, 0.0), sun).rgb;
    return a * 0.25;
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
    float3 ambient_top = sky_ambient(L.sun.xyz);

    // Per-frame jitter: interleaved gradient noise shifted along a
    // low-discrepancy (golden ratio) sequence.
    float frame = g_march.frame.x;
    float jitter = frac(interleaved_gradient_noise(float2(tid.xy) + 5.588238 * fmod(frame, 64.0)) +
                        0.6180340 * fmod(frame, 256.0));

    float r = ATMOS_VIEW_HEIGHT;
    float mu = dir.y;
    float bottom_r = ATMOS_GROUND_RADIUS + L.shell.x;
    float top_r = bottom_r + L.shell.y;
    float3 lum = float3(0.0, 0.0, 0.0);
    float trans = 1.0;
    float depth_sum = 0.0;
    float weight_sum = 0.0;
    float limit = scene_limit_km(tid.xy, dir);

    if (L.shell.z > 0.001 && r < bottom_r && mu > 0.0) {
        float t_in = shell_distance(r, mu, bottom_r);
        float t_out = shell_distance(r, mu, top_r);
        t_out = min(min(t_out, t_in + CLOUD_MAX_MARCH_KM), limit);
        if (t_out > t_in) {
            // Adaptive stepping (Schneider 2015): coarse steps through
            // empty space on the cheap density; on a hit, step back and
            // continue at a quarter of the step with the full density and
            // lighting until CLOUD_EMPTY_FINE samples in a row are empty.
            float base_steps = g_march.frame.y * lerp(2.0, 1.0, saturate(mu * 3.0));
            float coarse = (t_out - t_in) / base_steps;
            float fine = coarse * 0.25;
            float cos_sun = dot(dir, L.sun.xyz);
            float cos_moon = dot(dir, L.moon.xyz);
            float t = t_in + jitter * coarse;
            bool in_cloud = false;
            int empty = 0;
            for (int i = 0; i < CLOUD_MAX_STEPS; ++i) {
                if (t >= t_out || trans < 0.005) break;
                float dt = in_cloud ? fine : coarse;
                float alt = sqrt(r * r + t * t + 2.0 * r * t * mu) - ATMOS_GROUND_RADIUS;
                float3 p = float3(dir.x * t, alt, dir.z * t);
                if (!in_cloud) {
                    if (cloud_density(L, g_base, g_detail, g_curl, g_weather, g_noise_sampler, p,
                                      true) > 0.0) {
                        // Step back to where the cloud may begin.
                        in_cloud = true;
                        empty = 0;
                        t = max(t - coarse + fine * jitter, t_in);
                    } else {
                        t += coarse;
                    }
                    continue;
                }
                float d = cloud_density(L, g_base, g_detail, g_curl, g_weather, g_noise_sampler,
                                        p, false);
                if (d <= 0.0) {
                    if (++empty >= CLOUD_EMPTY_FINE) in_cloud = false;
                    t += dt;
                    continue;
                }
                empty = 0;
                float sigma_t = d * L.medium.x;
                float sigma_s = sigma_t * L.medium.y;
                float h = saturate((alt - L.shell.x) / L.shell.y);
                float3 s = sigma_s * cloud_ambient(L, ambient_top, h);
                float3 e_sun = cloud_light_irradiance(g_transmittance, g_lut_sampler, L.sun, alt);
                if (any(e_sun > 0.0)) {
                    float tau = cloud_light_depth(L, g_base, g_detail, g_curl, g_weather,
                                                  g_noise_sampler, p, L.sun.xyz, jitter);
                    s += cloud_light_scattering(L, e_sun, sigma_s, tau, cos_sun);
                }
                float3 e_moon = cloud_light_irradiance(g_transmittance, g_lut_sampler, L.moon, alt);
                if (any(e_moon > 0.0)) {
                    float tau = cloud_light_depth(L, g_base, g_detail, g_curl, g_weather,
                                                  g_noise_sampler, p, L.moon.xyz, jitter);
                    s += cloud_light_scattering(L, e_moon, sigma_s, tau, cos_moon);
                }
                float step_len = min(dt, t_out - t);
                float before = trans;
                lum += integrate_step(s, sigma_t, step_len, trans);
                float w = before - trans;
                depth_sum += (t + 0.5 * step_len) * w;
                weight_sum += w;
                t += dt;
            }
        }
    }
    // Cirrus behind the cumulus layer (never in front of an opaque surface
    // nearer than the layer).
    if (limit > 1.0e8 && trans > 0.005) {
        float4 ci = cirrus_layer(L, g_weather, g_curl, g_noise_sampler, g_transmittance,
                                 g_lut_sampler, dir, ambient_top);
        if (ci.a < 1.0) {
            float t_c = shell_distance(r, mu, ATMOS_GROUND_RADIUS + L.cirrus.y);
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
