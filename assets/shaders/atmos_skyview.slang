// atmos_skyview.slang — per-frame sky-view LUT (Phase 11, Slices 11.1/11.2).
//
// Raymarches single scattering (Rayleigh + Mie phase) with the transmittance
// LUT, adding the multiple-scattering term per step, then marches a volumetric
// cloud layer (1.5-4 km, value-noise FBM density with a sun shadow march and
// Henyey-Greenstein phase) composited over the sky. The alpha channel stores
// the cloud transmittance so the lighting pass can dim the analytic sun disc
// behind clouds. The result is a small lat-long image of the whole sky for the
// current sun direction. Clouds at LUT resolution are a distant cloudscape —
// full-parallax per-pixel volumetrics arrive with temporal reprojection later.

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

// ---------------------------------------------------------------------------
// Cloud layer (11.2)
// ---------------------------------------------------------------------------
static const float CLOUD_BOTTOM = ATMOS_GROUND_RADIUS + 1.5; // km
static const float CLOUD_TOP    = ATMOS_GROUND_RADIUS + 4.0; // km
static const float CLOUD_COVERAGE = 0.42;
static const float CLOUD_EXTINCTION = 24.0; // /km at full density
static const int   CLOUD_STEPS = 24;
static const int   CLOUD_SUN_STEPS = 5;

float hash2(float2 p) {
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float value_noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash2(i);
    float b = hash2(i + float2(1, 0));
    float c = hash2(i + float2(0, 1));
    float d = hash2(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm(float2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += amp * value_noise(p);
        p = p * 2.13 + float2(13.7, 9.1);
        amp *= 0.5;
    }
    return v;
}

// Cloud density at a point (planet-centred km, y up from the camera's column).
float cloud_density(float3 p, float altitude_km) {
    // Rounded vertical profile across the layer.
    float h01 = saturate((altitude_km - CLOUD_BOTTOM) / (CLOUD_TOP - CLOUD_BOTTOM));
    float profile = smoothstep(0.0, 0.18, h01) * (1.0 - smoothstep(0.55, 1.0, h01));

    float2 uv = p.xz * 0.16; // base shape scale (~6 km features)
    float base = fbm(uv);
    float detail = fbm(uv * 4.7 + 31.0);
    float coverage_t = 1.0 - CLOUD_COVERAGE;
    float d = saturate((base - coverage_t) / max(1.0 - coverage_t, 1e-3));
    d = saturate(d - detail * 0.28); // erode edges
    return d * profile;
}

float ray_shell_distance(float r, float mu, float shell_radius) {
    float disc = r * r * (mu * mu - 1.0) + shell_radius * shell_radius;
    if (disc < 0.0) return -1.0;
    return -r * mu + sqrt(disc);
}

// Henyey-Greenstein phase.
float hg_phase(float cos_theta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * ATMOS_PI * pow(1.0 + g2 - 2.0 * g * cos_theta, 1.5));
}

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

    // ------------------------------------------------------------------
    // Volumetric cloud layer (11.2): march entry->exit through the shell,
    // Beer-Lambert absorption, sun shadow march, HG phase + ambient.
    // ------------------------------------------------------------------
    float cloud_trans = 1.0;
    float3 cloud_lum = float3(0.0, 0.0, 0.0);
    if (!ground) {
        float t_in = ray_shell_distance(r, mu, CLOUD_BOTTOM);
        float t_out = ray_shell_distance(r, mu, CLOUD_TOP);
        if (t_out > 0.0) {
            t_in = max(t_in, 0.0);
            float march_len = t_out - t_in;
            // Skip near-horizon slivers with absurd path lengths.
            if (march_len > 0.0 && march_len < 60.0) {
                float dt_c = march_len / CLOUD_STEPS;
                // Cloud luminance lives INSIDE the LUT and gets the same
                // SKY_LUT_BRIGHTNESS scale as the sky — keep it in LUT units.
                float3 sun_trans_atmos =
                    sample_transmittance(g_transmittance, g_samp,
                                         clamp(r + 2.0, ATMOS_GROUND_RADIUS, ATMOS_TOP_RADIUS),
                                         mu_sun_at);
                float3 sun_energy = sun_trans_atmos * 0.45;
                float3 ambient = sample_multiscatter(g_multiscatter, g_samp, r, mu_sun_at) *
                                     6.0 +
                                 float3(0.010, 0.014, 0.020);
                float phase = lerp(hg_phase(cos_theta, 0.55), hg_phase(cos_theta, -0.2), 0.3);
                // Aerial perspective on the clouds themselves: distant cloud
                // banks fade into the sky instead of forming a hard wall at
                // the horizon.
                float distance_fade = exp(-t_in * 0.045);

                for (int i = 0; i < CLOUD_STEPS; ++i) {
                    if (cloud_trans < 0.01) break;
                    float t = t_in + (float(i) + 0.5) * dt_c;
                    float3 p = dir * t;
                    float altitude = sqrt(r * r + t * t + 2.0 * r * t * mu);
                    float dens = cloud_density(p, altitude);
                    if (dens <= 0.001) continue;

                    float sigma = dens * CLOUD_EXTINCTION;
                    // Short march toward the sun for self-shadowing.
                    float shadow_depth = 0.0;
                    float ds = (CLOUD_TOP - CLOUD_BOTTOM) / float(CLOUD_SUN_STEPS);
                    for (int s = 1; s <= CLOUD_SUN_STEPS; ++s) {
                        float3 sp = p + sun_local * (float(s) * ds);
                        float s_alt = altitude + sun_local.y * float(s) * ds;
                        shadow_depth += cloud_density(sp, s_alt) * ds;
                    }
                    float light = exp(-shadow_depth * CLOUD_EXTINCTION * 0.6);
                    // Powder term: brightens deep, freshly lit cloud interiors.
                    float powder = 1.0 - exp(-2.0 * sigma * dt_c);

                    float3 scatter = sun_energy * light * (phase + 0.12) *
                                         (0.7 + 0.3 * powder) +
                                     ambient;
                    float step_trans = exp(-sigma * dt_c);
                    cloud_lum += cloud_trans * scatter * (1.0 - step_trans);
                    cloud_trans *= step_trans;
                }
                cloud_lum *= distance_fade;
                cloud_trans = lerp(1.0, cloud_trans, distance_fade);
            }
        }
    }
    luminance = luminance * cloud_trans + cloud_lum * throughput;

    // Alpha = cloud transmittance: the lighting pass dims the analytic sun
    // disc behind clouds with it.
    g_out[tid.xy] = float4(luminance, cloud_trans);
}
