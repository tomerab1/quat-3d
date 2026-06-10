// atmos_skyview.slang — per-frame sky-view LUT (Phase 11, Slice 11.1).
//
// Raymarches single scattering (Rayleigh + Mie phase) with the transmittance
// LUT, adding the multiple-scattering term per step. The result is a small
// lat-long image of the whole sky for the current sun direction, which the
// lighting pass samples per pixel. Cheap enough to refresh every frame.

import atmosphere;

struct PushConstants {
    float4 sun_direction; // xyz: TOWARD the sun (y up), w unused
}
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0)]] Texture2D           g_transmittance;
[[vk::binding(1)]] Texture2D           g_multiscatter;
[[vk::binding(2)]] SamplerState        g_samp;
[[vk::binding(3)]] RWTexture2D<float4> g_out;

static const int STEPS = 32;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint width = 0;
    uint height = 0;
    g_out.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;

    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    float view_zenith = 0.0;
    float light_azimuth = 0.0;
    skyview_params_from_uv(uv, view_zenith, light_azimuth);

    float r = ATMOS_VIEW_HEIGHT;
    float3 sun = normalize(pc.sun_direction.xyz);
    float sun_zenith_cos = sun.y;
    float sun_zenith_sin = sqrt(saturate(1.0 - sun_zenith_cos * sun_zenith_cos));

    // View direction in a frame where the sun's azimuth is +X.
    float3 dir = float3(sin(view_zenith) * cos(light_azimuth), cos(view_zenith),
                        sin(view_zenith) * sin(light_azimuth));
    float3 sun_local = float3(sun_zenith_sin, sun_zenith_cos, 0.0);

    float mu = dir.y;
    bool ground = intersects_ground(r, mu);
    float d = ground ? distance_to_ground(r, mu) : distance_to_top(r, mu);
    float dt = d / STEPS;

    float cos_theta = dot(dir, sun_local);
    float phase_r = rayleigh_phase(cos_theta);
    float phase_m = mie_phase(cos_theta);

    float3 luminance = float3(0.0, 0.0, 0.0);
    float3 throughput = float3(1.0, 1.0, 1.0);

    // The sun's zenith cosine seen from sample points along this ray.
    float mu_sun_at = sun_zenith_cos;
    for (int i = 0; i < STEPS; ++i) {
        float t = (float(i) + 0.5) * dt;
        float rt = sqrt(r * r + t * t + 2.0 * r * t * mu);
        rt = clamp(rt, ATMOS_GROUND_RADIUS + 1e-4, ATMOS_TOP_RADIUS);
        float h = rt - ATMOS_GROUND_RADIUS;
        MediumSample m = sample_medium(h);
        float3 step_trans = exp(-m.extinction * dt);

        // Sun zenith at the sample: the camera sits at ground level and rays
        // are short relative to the planet radius, so the camera's value is an
        // excellent approximation (Hillaire's demo does the same for the
        // ground-level sky-view).
        float mu_sun_t = mu_sun_at;
        float3 sun_trans = intersects_ground(rt, mu_sun_t)
                               ? float3(0.0, 0.0, 0.0)
                               : sample_transmittance(g_transmittance, g_samp, rt, mu_sun_t);
        float3 multi = sample_multiscatter(g_multiscatter, g_samp, rt, mu_sun_t);

        float3 scatter_phased =
            m.rayleigh_scatter * phase_r + m.mie_scatter.xxx * phase_m;
        float3 scatter_total = m.rayleigh_scatter + m.mie_scatter.xxx;

        // Per-step analytic integration (Hillaire): single scatter with the
        // real phase + multiple scattering with the isotropic LUT term.
        float3 s = scatter_phased * sun_trans + scatter_total * multi;
        float3 s_int = (s - s * step_trans) / max(m.extinction, 1e-6);

        luminance += throughput * s_int;
        throughput *= step_trans;
    }

    // Ground-hitting rays: add the sun-lit ground (Lambert, fixed albedo) seen
    // through the accumulated transmittance, so the lower hemisphere reads as
    // terrain haze instead of empty scattering.
    if (ground) {
        float3 sun_trans = sun_zenith_cos > 0.0
                               ? sample_transmittance(g_transmittance, g_samp,
                                                      ATMOS_GROUND_RADIUS + 1e-3, sun_zenith_cos)
                               : float3(0.0, 0.0, 0.0);
        const float3 ground_albedo = float3(0.40, 0.35, 0.28);
        luminance += throughput * sun_trans * max(sun_zenith_cos, 0.0) * ground_albedo /
                     ATMOS_PI;
    }

    g_out[tid.xy] = float4(luminance, 1.0);
}
