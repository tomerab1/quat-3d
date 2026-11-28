// clouds.slang — the volumetric cloud model (ROADMAP R13.4).
//
// Shape: Schneider's Nubis — "The Real-time Volumetric Cloudscapes of
// Horizon: Zero Dawn" (SIGGRAPH 2015 Advances), GPU Pro 7 "Real-Time
// Volumetric Cloudscapes" (2016) and "Nubis: Authoring Real-Time Volumetric
// Cloudscapes with the Decima Engine" (SIGGRAPH 2017 Advances). A
// weather-driven coverage / type / precipitation field over the world,
// type-dependent height profiles (stratus, stratocumulus, cumulus), a
// Perlin-Worley base shape eroded by curl-distorted Worley detail (wispy
// at the base, billowy at the top). The coverage and type come from the
// frame's weather (R13.1): the baked weather noise only varies them around
// the weather's values, so no re-bake is ever needed.
//
// Light: Hillaire, "Physically Based Sky, Atmosphere and Cloud Rendering in
// Frostbite" (SIGGRAPH 2016 course): energy-conserving integration of the
// scattered light over each step (from Hillaire, "Physically Based and
// Unified Volumetric Rendering in Frostbite", SIGGRAPH 2015 Advances), a
// dual-lobe Henyey-Greenstein phase function and multiple scattering
// approximated by Wrenninge, Kulla & Lundqvist's octaves ("Oz: The Great
// and Volumetric", SIGGRAPH 2013 talk): octave i scatters a^i of the light
// through b^i of the optical depth with phase anisotropy c^i (a <= b keeps it
// energy-conserving). Every light — the sun, and the moon from R22.1's
// SkyState — reaches the cloud through the atmosphere's transmittance LUT
// (Hillaire 2020), with its own extra-atmospheric irradiance.
//
// Units: kilometres and 1/km. Positions are camera-relative (x, z) with the
// altitude above the ground; the camera stands at ATMOS_VIEW_HEIGHT on the
// planet (lib/atmosphere.slang). Luminance is per unit extra-atmospheric
// solar irradiance, the sky-view LUT's unit. The CPU mirror of the light
// integration is engine::renderer::cloud_reference (cloud_reference.hpp).

import atmosphere;
import cloud_noise;

// The cloud layer of one frame (std140, 160 bytes; engine::renderer::CloudLayerGpu).
public struct CloudLayer {
    public float4 shell;       // x base altitude km, y thickness km, z coverage 0..1, w type 0..1
    public float4 medium;      // x extinction /km at density 1, y albedo, z detail erosion, w precipitation
    public float4 scale;       // x 1/base tile km, y 1/detail tile km, z 1/weather tile km, w curl km
    public float4 offset;      // xy base field offset km (world anchor + drift), zw detail offset km
    public float4 cirrus;      // x coverage, y altitude km, z optical depth at full cover, w 1/tile km
    public float4 cirrus_offset; // xy cirrus offset km, z detail drift km, w unused
    public float4 octaves;     // multiple scattering: x a, y b, z c, w octave count
    public float4 phase;       // x forward g, y backward g, z backward weight, w ambient scale
    public float4 sun;         // xyz unit toward the sun, w extra-atmospheric irradiance (rel.)
    public float4 moon;        // xyz unit toward the moon, w extra-atmospheric irradiance (rel.)
}

public static const int CLOUD_MAX_OCTAVES = 4;

// Henyey-Greenstein phase function (normalised over the sphere).
public float hg_phase(float cos_theta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * ATMOS_PI * pow(max(1.0 + g2 - 2.0 * g * cos_theta, 1e-6), 1.5));
}

// The dual-lobe phase of octave `i` (anisotropy scaled by c^i).
public float cloud_phase(CloudLayer L, float cos_theta, float c_pow) {
    return lerp(hg_phase(cos_theta, L.phase.x * c_pow), hg_phase(cos_theta, L.phase.y * c_pow),
                L.phase.z);
}

// Type-dependent height profile (GPU Pro 7): stratus hug the base,
// stratocumulus fill the lower half, cumulus tower to the top. `h` is the
// height fraction in the shell, `type` 0 stratus .. 0.5 stratocumulus .. 1
// cumulus.
public float cloud_height_profile(float h, float type) {
    float stratus = saturate(remap(h, 0.0, 0.07, 0.0, 1.0)) * saturate(remap(h, 0.15, 0.3, 1.0, 0.0));
    float strato = saturate(remap(h, 0.0, 0.2, 0.0, 1.0)) * saturate(remap(h, 0.35, 0.65, 1.0, 0.0));
    float cumulus = saturate(remap(h, 0.0, 0.1, 0.0, 1.0)) * saturate(remap(h, 0.6, 0.95, 1.0, 0.0));
    return type < 0.5 ? lerp(stratus, strato, type * 2.0) : lerp(strato, cumulus, type * 2.0 - 1.0);
}

// Local weather at world-anchored xz (km): x coverage, y type, z
// precipitation. Two lookups of the weather noise at unrelated scales and
// orientations hide its tiling.
public float3 cloud_weather(CloudLayer L, Texture2D weather, SamplerState samp, float2 xz) {
    float2 p = xz + L.offset.xy;
    float4 w0 = weather.SampleLevel(samp, p * L.scale.z, 0.0);
    float2 q = float2(0.8 * p.x - 0.6 * p.y, 0.6 * p.x + 0.8 * p.y) * (L.scale.z * 2.71);
    float4 w1 = weather.SampleLevel(samp, q + 0.37, 0.0);
    // The field is ~0.5 on average; spread the coverage around the weather's
    // value, less so near clear and overcast skies.
    float field = w0.r * 0.7 + w1.r * 0.3 - 0.5;
    float global = L.shell.z;
    float spread = 2.2 * min(global, 1.0 - global) + 0.15;
    float coverage = saturate(global + field * spread);
    float type = saturate(L.shell.w + (w0.g - 0.5) * 0.5);
    float precip = saturate(L.medium.w * (0.4 + 1.2 * w1.b));
    return float3(coverage, type, precip);
}

// Cloud density (0..~1, scaled by the medium's extinction later) at
// camera-relative position `p` (xz km, y = altitude above ground km).
// `cheap` skips the detail erosion (empty-space stepping and light marches
// far from the sample).
public float cloud_density(CloudLayer L, Texture3D base, Texture3D detail, Texture2D curl,
                           Texture2D weather, SamplerState samp, float3 p, bool cheap) {
    float h = (p.y - L.shell.x) / L.shell.y;
    if (h <= 0.0 || h >= 1.0) return 0.0;
    float3 w = cloud_weather(L, weather, samp, p.xz);
    if (w.x <= 0.001) return 0.0;
    float profile = cloud_height_profile(h, w.y);
    if (profile <= 0.0) return 0.0;

    float3 anchored = float3(p.x + L.offset.x, p.y, p.z + L.offset.y);
    float4 b = base.SampleLevel(samp, anchored * L.scale.x, 0.0);
    float low_fbm = b.g * 0.625 + b.b * 0.25 + b.a * 0.125;
    float shape = saturate(remap(saturate(remap(b.r, low_fbm - 1.0, 1.0, 0.0, 1.0)),
                                 CLOUD_SHAPE_LO, CLOUD_SHAPE_HI, 0.0, 1.0)) * profile;
    // Coverage carves the shape and scales what remains (Schneider 2015):
    // sparse skies get small, thin cumuli rather than faint giant ones.
    float d = saturate(remap(shape, 1.0 - w.x, 1.0, 0.0, 1.0)) * w.x;
    if (d <= 0.0 || cheap) return d * (1.0 + 2.0 * w.z);

    // Curl-distorted detail, stronger toward the base (wisps).
    float2 c = curl.SampleLevel(samp, anchored.xz * L.scale.x * 4.0, 0.0).rg * 2.0 - 1.0;
    float3 q = float3(anchored.x + L.offset.z, p.y, anchored.z + L.offset.w);
    q.xz += c * L.scale.w * (1.0 - h);
    float4 dn = detail.SampleLevel(samp, q * L.scale.y, 0.0);
    float high_fbm = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
    float erode = lerp(high_fbm, 1.0 - high_fbm, saturate(h * 6.0));
    d = saturate(remap(d, erode * L.medium.z, 1.0, 0.0, 1.0));
    // Rain clouds are optically thicker (larger droplets, more water).
    return d * (1.0 + 2.0 * w.z);
}

// Extra-atmospheric irradiance of a light reaching altitude `alt_km` through
// the atmosphere (the transmittance LUT; 0 when the planet blocks it).
public float3 cloud_light_irradiance(Texture2D transmittance_lut, SamplerState lut_samp,
                                     float4 light, float alt_km) {
    if (light.w <= 0.0) return float3(0.0, 0.0, 0.0);
    float r = clamp(ATMOS_GROUND_RADIUS + alt_km, ATMOS_GROUND_RADIUS, ATMOS_TOP_RADIUS);
    if (intersects_ground(r, light.y)) return float3(0.0, 0.0, 0.0);
    return light.w * sample_transmittance(transmittance_lut, lut_samp, r, light.y);
}

// In-scattered radiance (per unit length, before the step integration) of
// one light at a sample whose optical depth toward it is `tau`: the octave
// sum of Wrenninge et al. 2013. `sigma_s` is the scattering coefficient.
public float3 cloud_light_scattering(CloudLayer L, float3 irradiance, float sigma_s, float tau,
                                     float cos_theta) {
    float3 s = float3(0.0, 0.0, 0.0);
    float a = 1.0;
    float b = 1.0;
    float c = 1.0;
    int octaves = int(L.octaves.w);
    for (int i = 0; i < CLOUD_MAX_OCTAVES; ++i) {
        if (i >= octaves) break;
        s += irradiance * (sigma_s * a * cloud_phase(L, cos_theta, c) * exp(-tau * b));
        a *= L.octaves.x;
        b *= L.octaves.y;
        c *= L.octaves.z;
    }
    return s;
}

// Energy-conserving step integration (Hillaire 2015, eq. in sec. 5.6.3):
// in-scattered radiance S constant over a step of extinction sigma_t and
// length dt contributes S (1 - exp(-sigma_t dt)) / sigma_t, attenuated by the
// transmittance up to the step; returns that and updates `trans`.
public float3 integrate_step(float3 s, float sigma_t, float dt, inout float trans) {
    float step_trans = exp(-sigma_t * dt);
    float3 contribution = sigma_t > 1e-6 ? (s - s * step_trans) / sigma_t : s * dt;
    float3 out_l = trans * contribution;
    trans *= step_trans;
    return out_l;
}

// The light march: optical depth toward the light from `p` (km). Six samples
// at doubling distances inside the shell (Schneider 2015's cone, without the
// lateral offsets — the temporal accumulation supplies the jitter), the last
// two cheap, plus one long-range cheap sample for the shadow of distant
// towers. `jitter` (0..1) shifts the samples within their steps.
public static const int CLOUD_LIGHT_SAMPLES = 6;

public float cloud_light_depth(CloudLayer L, Texture3D base, Texture3D detail, Texture2D curl,
                               Texture2D weather, SamplerState samp, float3 p, float3 to_light,
                               float jitter) {
    float tau = 0.0;
    float t = 0.0;
    float ds = 0.04 * L.shell.y; // first step: 4 % of the shell
    for (int i = 0; i < CLOUD_LIGHT_SAMPLES; ++i) {
        float3 q = p + to_light * (t + ds * (0.5 + 0.5 * (jitter - 0.5)));
        tau += cloud_density(L, base, detail, curl, weather, samp, q, i >= 4) * ds;
        t += ds;
        ds *= 2.0;
    }
    float3 far_q = p + to_light * (t + 0.5 * L.shell.y);
    tau += cloud_density(L, base, detail, curl, weather, samp, far_q, true) * 0.5 * L.shell.y;
    return tau * L.medium.x;
}

// Isotropic ambient radiance at height fraction h from the sky above (top)
// fading toward the darker light under the layer (bottom).
public float3 cloud_ambient(CloudLayer L, float3 ambient_top, float h) {
    return ambient_top * (L.phase.w * lerp(0.4, 1.0, saturate(h)));
}

// ---------------------------------------------------------------------------
// Cirrus (high, thin ice cloud). A 2D layer at L.cirrus.y km: optical depth
// from the weather noise stretched along the wind and curl-distorted into
// streaks, lit by single scattering of a plane-parallel slab — Chandrasekhar,
// "Radiative Transfer" (1960): L = E w p mu_s / (mu_s + mu_v) (1 - exp(-tau
// (1/mu_s + 1/mu_v))) — plus the ambient sky. Returns rgb luminance and its
// transmittance in a.
// ---------------------------------------------------------------------------
public float4 cirrus_layer(CloudLayer L, Texture2D weather, Texture2D curl, SamplerState samp,
                           Texture2D transmittance_lut, SamplerState lut_samp, float3 dir,
                           float3 ambient_top) {
    if (L.cirrus.x <= 0.001 || dir.y <= 0.01) return float4(0.0, 0.0, 0.0, 1.0);
    float r = ATMOS_VIEW_HEIGHT;
    float shell = ATMOS_GROUND_RADIUS + L.cirrus.y;
    float disc = r * r * (dir.y * dir.y - 1.0) + shell * shell;
    float t = -r * dir.y + sqrt(max(disc, 0.0));
    float2 xz = dir.xz * t + L.cirrus_offset.xy;
    float2 c = curl.SampleLevel(samp, xz * L.cirrus.w * 0.5, 0.0).rg * 2.0 - 1.0;
    // Streaks: stretch across the drift direction.
    float2 uv = float2(xz.x * L.cirrus.w, xz.y * L.cirrus.w * 4.0) + c * 0.08;
    float4 w = weather.SampleLevel(samp, uv, 0.0);
    float4 w2 = weather.SampleLevel(samp, uv * 3.1 + L.cirrus_offset.z * L.cirrus.w, 0.0);
    float field = saturate(remap(w.b * 0.7 + w2.g * 0.3, 1.0 - L.cirrus.x, 1.0, 0.0, 1.0));
    float tau = field * L.cirrus.z;
    if (tau <= 1e-4) return float4(0.0, 0.0, 0.0, 1.0);
    float mu_v = max(sqrt(max(disc, 0.0)) / shell, 0.02); // cosine at the layer
    float3 lum = float3(0.0, 0.0, 0.0);
    float4 lights[2] = { L.sun, L.moon };
    for (int i = 0; i < 2; ++i) {
        float3 e = cloud_light_irradiance(transmittance_lut, lut_samp, lights[i], L.cirrus.y);
        float mu_s = max(lights[i].y, 0.02);
        if (all(e <= 0.0)) continue;
        // Ice crystals: strongly forward-scattering (halo lobes not modelled).
        float p = lerp(hg_phase(dot(dir, lights[i].xyz), 0.75), hg_phase(dot(dir, lights[i].xyz), -0.2), 0.2);
        lum += e * (p * mu_s / (mu_s + mu_v) * (1.0 - exp(-tau * (1.0 / mu_s + 1.0 / mu_v))));
    }
    float trans = exp(-tau / mu_v);
    lum += ambient_top * L.phase.w * (1.0 - trans);
    return float4(lum, trans);
}

// ---------------------------------------------------------------------------
// The march of one view ray through the layer (the cloud pass and the IBL
// probe). From the camera (ATMOS_VIEW_HEIGHT) along `dir` through the
// spherical shell, at most CLOUD_MAX_MARCH_KM and `limit_km`. Adaptive
// stepping (Schneider 2015): coarse steps through empty space on the cheap
// density; on a hit, step back and continue at a quarter of the step with
// the full density and lighting until CLOUD_EMPTY_FINE samples in a row are
// empty. `base_steps` coarse steps span a vertical ray (twice as many toward
// the horizon); `jitter` (0..1) offsets the start and the light samples.
// Returns the luminance, the transmittance and the transmittance-weighted
// distance (depth_sum / weight_sum, km).
// ---------------------------------------------------------------------------
public static const int   CLOUD_MAX_STEPS = 192; // iterations (coarse + fine) per ray
public static const int   CLOUD_EMPTY_FINE = 6;  // empty fine samples before stepping coarse again
public static const float CLOUD_MAX_MARCH_KM = 60.0;

public struct CloudRay {
    public float3 lum;
    public float  trans;
    public float  depth_sum;
    public float  weight_sum;
}

public float cloud_layer_distance(float r, float mu, float shell_radius) {
    float disc = r * r * (mu * mu - 1.0) + shell_radius * shell_radius;
    if (disc < 0.0) return -1.0;
    return -r * mu + sqrt(disc);
}

public CloudRay march_cloud_layer(CloudLayer L, Texture3D g_base, Texture3D g_detail,
                                  Texture2D g_curl, Texture2D g_weather,
                                  SamplerState g_noise_sampler, Texture2D g_transmittance,
                                  SamplerState g_lut_sampler, float3 dir, float limit,
                                  float jitter, float3 ambient_top, float steps) {
    CloudRay out;
    out.lum = float3(0.0, 0.0, 0.0);
    out.trans = 1.0;
    out.depth_sum = 0.0;
    out.weight_sum = 0.0;
    float r = ATMOS_VIEW_HEIGHT;
    float mu = dir.y;
    float bottom_r = ATMOS_GROUND_RADIUS + L.shell.x;
    float top_r = bottom_r + L.shell.y;
    if (L.shell.z <= 0.001 || r >= bottom_r || mu <= 0.0) return out;
    float t_in = cloud_layer_distance(r, mu, bottom_r);
    float t_out = cloud_layer_distance(r, mu, top_r);
    t_out = min(min(t_out, t_in + CLOUD_MAX_MARCH_KM), limit);
    if (t_out <= t_in) return out;
    float3 lum = float3(0.0, 0.0, 0.0);
    float trans = 1.0;
    float depth_sum = 0.0;
    float weight_sum = 0.0;
    float base_steps = steps * lerp(2.0, 1.0, saturate(mu * 3.0));
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
            if (cloud_density(L, g_base, g_detail, g_curl, g_weather, g_noise_sampler, p, true) >
                0.0) {
                // Step back to where the cloud may begin.
                in_cloud = true;
                empty = 0;
                t = max(t - coarse + fine * jitter, t_in);
            } else {
                t += coarse;
            }
            continue;
        }
        float d = cloud_density(L, g_base, g_detail, g_curl, g_weather, g_noise_sampler, p, false);
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
            float tau = cloud_light_depth(L, g_base, g_detail, g_curl, g_weather, g_noise_sampler,
                                          p, L.sun.xyz, jitter);
            s += cloud_light_scattering(L, e_sun, sigma_s, tau, cos_sun);
        }
        float3 e_moon = cloud_light_irradiance(g_transmittance, g_lut_sampler, L.moon, alt);
        if (any(e_moon > 0.0)) {
            float tau = cloud_light_depth(L, g_base, g_detail, g_curl, g_weather, g_noise_sampler,
                                          p, L.moon.xyz, jitter);
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
    out.lum = lum;
    out.trans = trans;
    out.depth_sum = depth_sum;
    out.weight_sum = weight_sum;
    return out;
}

// ---------------------------------------------------------------------------
// The sun's transmittance through the layer (cloud shadow map, R13.1): the
// optical depth along the line from camera-relative `offset_km` toward
// `sun_dir` through the shell (midpoint rule, `steps` samples, at most
// CLOUD_SHADOW_PATH_KM), with the first octave's depth scale b so that the
// shadows carry the same multiple-scattering softening the clouds are lit
// with.
// ---------------------------------------------------------------------------
public static const float CLOUD_SHADOW_PATH_KM = 80.0;

public float cloud_shadow_transmittance(CloudLayer L, Texture3D base, Texture3D detail,
                                        Texture2D curl, Texture2D weather, SamplerState samp,
                                        float3 offset_km, float3 sun_dir, int steps) {
    if (L.shell.z <= 0.001) return 1.0;
    float y0 = offset_km.y;
    float bottom = L.shell.x;
    float top = bottom + L.shell.y;
    if (sun_dir.y <= 1e-3) return 1.0;
    float t_in = max((bottom - y0) / sun_dir.y, 0.0);
    float t_out = (top - y0) / sun_dir.y;
    float len = min(t_out - t_in, CLOUD_SHADOW_PATH_KM);
    if (len <= 0.0) return 1.0;
    float dt = len / float(steps);
    float depth = 0.0;
    for (int i = 0; i < steps; ++i) {
        float3 p = offset_km + sun_dir * (t_in + (float(i) + 0.5) * dt);
        depth += cloud_density(L, base, detail, curl, weather, samp, p, false) * dt;
    }
    // Octave 0 transmittance plus the forward-scattered octaves that still
    // reach the ground (diffuse light under the cloud).
    float tau = depth * L.medium.x;
    float t = 0.0;
    float a = 1.0;
    float b = 1.0;
    float norm = 0.0;
    int octaves = int(L.octaves.w);
    for (int i = 0; i < CLOUD_MAX_OCTAVES; ++i) {
        if (i >= octaves) break;
        t += a * exp(-tau * b);
        norm += a;
        a *= L.octaves.x;
        b *= L.octaves.y;
    }
    return t / max(norm, 1e-6);
}
