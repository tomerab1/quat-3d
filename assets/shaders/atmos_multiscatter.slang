// atmos_multiscatter.slang — multiple-scattering LUT (Phase 11, Slice 11.1).
//
// Hillaire's isotropic multiple-scattering approximation: for every
// (sun-zenith, altitude) pair, integrate single in-scatter L2 and transfer f_ms
// over the sphere of directions, then Psi_ms = L2 / (1 - f_ms). Static.

import atmosphere;

[[vk::binding(0)]] Texture2D            g_transmittance;
[[vk::binding(1)]] SamplerState         g_samp;
[[vk::binding(2)]] RWTexture2D<float4>  g_out;

static const int DIR_SQRT = 8;  // 64 sphere directions
static const int STEPS = 20;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint width = 0;
    uint height = 0;
    g_out.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;

    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    float mu_sun = uv.x * 2.0 - 1.0;
    float r = ATMOS_GROUND_RADIUS + uv.y * (ATMOS_TOP_RADIUS - ATMOS_GROUND_RADIUS);
    r = clamp(r, ATMOS_GROUND_RADIUS + 0.01, ATMOS_TOP_RADIUS - 0.01);
    float3 sun_dir = float3(sqrt(saturate(1.0 - mu_sun * mu_sun)), mu_sun, 0.0);

    float3 luminance = float3(0.0, 0.0, 0.0); // L2: in-scattered light
    float3 transfer = float3(0.0, 0.0, 0.0);  // f_ms: energy kept for the next bounce

    // Uniform sphere of ray directions.
    for (int i = 0; i < DIR_SQRT; ++i) {
        for (int j = 0; j < DIR_SQRT; ++j) {
            float theta = ATMOS_PI * (float(i) + 0.5) / float(DIR_SQRT);
            float phi = 2.0 * ATMOS_PI * (float(j) + 0.5) / float(DIR_SQRT);
            float3 dir = float3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
            float sphere_weight = sin(theta) * (ATMOS_PI / float(DIR_SQRT)) *
                                  (2.0 * ATMOS_PI / float(DIR_SQRT));

            float mu = dir.y;
            bool ground = intersects_ground(r, mu);
            float d = ground ? distance_to_ground(r, mu) : distance_to_top(r, mu);
            float dt = d / STEPS;

            float3 throughput = float3(1.0, 1.0, 1.0);
            float cos_sun = dot(dir, sun_dir);
            // Isotropic phase for the multiple-scatter estimate (Hillaire).
            float phase = 1.0 / (4.0 * ATMOS_PI);

            for (int s = 0; s < STEPS; ++s) {
                float t = (float(s) + 0.5) * dt;
                float rt = sqrt(r * r + t * t + 2.0 * r * t * mu);
                rt = clamp(rt, ATMOS_GROUND_RADIUS, ATMOS_TOP_RADIUS);
                float h = rt - ATMOS_GROUND_RADIUS;
                MediumSample m = sample_medium(h);
                float3 scatter = m.rayleigh_scatter + m.mie_scatter;
                float3 step_trans = exp(-m.extinction * dt);

                // Sun visibility/transmittance at the sample point.
                float mu_sun_t = clamp((r * mu_sun + t * cos_sun) / rt, -1.0, 1.0);
                float3 sun_trans = intersects_ground(rt, mu_sun_t)
                                       ? float3(0.0, 0.0, 0.0)
                                       : sample_transmittance(g_transmittance, g_samp, rt,
                                                              mu_sun_t);

                // Analytic integral of scattering over the step (Hillaire).
                float3 s_int =
                    (scatter * phase * sun_trans - scatter * phase * sun_trans * step_trans) /
                    max(m.extinction, 1e-6);
                float3 f_int = (scatter - scatter * step_trans) / max(m.extinction, 1e-6);

                luminance += throughput * s_int * sphere_weight;
                transfer += throughput * f_int * sphere_weight;
                throughput *= step_trans;
            }

            // Ground albedo bounce.
            if (ground) {
                float rt = ATMOS_GROUND_RADIUS;
                float mu_sun_t = clamp((r * mu_sun + d * cos_sun) / rt, -1.0, 1.0);
                if (mu_sun_t > 0.0) {
                    float3 sun_trans =
                        sample_transmittance(g_transmittance, g_samp, rt, mu_sun_t);
                    const float3 ground_albedo = float3(0.3, 0.3, 0.3);
                    luminance += throughput * sun_trans * mu_sun_t * ground_albedo /
                                 ATMOS_PI * sphere_weight;
                }
            }
        }
    }

    float inv_4pi = 1.0 / (4.0 * ATMOS_PI);
    luminance *= inv_4pi;
    transfer *= inv_4pi;
    float3 psi = luminance / max(float3(1.0, 1.0, 1.0) - transfer, 1e-4);
    g_out[tid.xy] = float4(psi, 1.0);
}
