// atmos_skyview.slang — per-frame sky-view LUT (Phase 11, Slice 11.1).
//
// Raymarches single scattering (Rayleigh + Mie phase) with the transmittance
// LUT, adding the multiple-scattering term per step. The result is a small
// lat-long image of the ATMOSPHERE for the current sun direction, with the
// lit planet surface below the horizon (R22.2.1: sun, Moon and night-sky
// irradiance through lib/atmosphere.slang's ground model). Clouds are
// deliberately NOT in this LUT — its resolution smears them; consumers march
// them per pixel via march_clouds() in lib/atmosphere.slang (11.2).

import atmosphere;

// Mirrors SkyViewPush in atmosphere_pass.cpp.
struct PushConstants {
    float4 sun_direction;  // xyz: TOWARD the sun (y up), w: its extra-atmospheric
                           // irradiance relative to the LUT's unit sun (1)
    float4 moon_direction; // xyz: toward the Moon, w: its relative irradiance
    float4 night;          // x: night-sky floor irradiance (relative)
}
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0)]] Texture2D           g_transmittance;
[[vk::binding(1)]] Texture2D           g_multiscatter;
[[vk::binding(2)]] SamplerState        g_samp;
[[vk::binding(3)]] [format("rgba16f")] RWTexture2D<float4> g_out;
[[vk::binding(4)]] Texture2D           g_sky_irradiance; // RGBA32F, Load only

static const int STEPS = 32;

// Irradiance on the ground from every light (relative to the unit sun).
float3 ground_irradiance(float3 sun) {
    float3 e = ground_irradiance_from(g_transmittance, g_sky_irradiance, g_samp, sun.y,
                                      pc.sun_direction.w);
    float3 moon = normalize(pc.moon_direction.xyz);
    e += ground_irradiance_from(g_transmittance, g_sky_irradiance, g_samp, moon.y,
                                pc.moon_direction.w);
    return e + pc.night.x;
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
    float3 throughput;
    float3 luminance =
        integrate_inscattering(g_transmittance, g_multiscatter, g_samp, r, mu, sun_zenith_cos,
                               dot(dir, sun_local), d, STEPS, throughput) *
        pc.sun_direction.w;

    // The whole image is scaled so the brightest plausible value stays near
    // 1 whatever the light level (RGBA16F keeps ~11 bits from 6e-5 up): the
    // ground irradiance is the reference. Every thread computes the same
    // scale; alpha carries log2(1 / scale) for decode_skyview.
    float3 e_ground = ground_irradiance(sun);
    float reference = max(max(e_ground.r, max(e_ground.g, e_ground.b)), 1e-30);
    float log_inv_scale = round(log2(reference)); // whole stops: exact in fp16

    // Ground-hitting rays: the Lambertian planet surface seen through the
    // transmittance accumulated to it (its in-scattering is already in
    // `luminance`: aerial perspective).
    if (ground) {
        luminance += throughput * e_ground * ATMOS_GROUND_ALBEDO / ATMOS_PI;
    } else {
        // The natural night sky (stars below the catalogue, airglow,
        // zodiacal light) as the uniform dome that puts the night floor's
        // illuminance on the ground: L = E / pi, ~3e-4 cd/m² (21.7
        // mag/arcsec², a dark site's zenith) for 1e-3 lux. R22.2.4 replaces
        // it with airglow and zodiacal light.
        luminance += pc.night.x / ATMOS_PI;
    }

    g_out[tid.xy] = float4(luminance * exp2(-log_inv_scale), log_inv_scale);
}
