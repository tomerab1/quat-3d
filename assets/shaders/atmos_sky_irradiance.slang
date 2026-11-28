// atmos_sky_irradiance.slang — sky irradiance LUT (R22.2.1).
//
// For each zenith cosine of a light (texel x, lib/atmosphere.slang
// SKY_IRRADIANCE_*), the irradiance a horizontal patch of ground receives
// from the sky that light illuminates: the cosine-weighted integral of the
// in-scattered luminance (single + multiple scattering, Hillaire 2020) over
// the upper hemisphere, per unit extra-atmospheric irradiance. This is the
// ground-irradiance texture of Bruneton & Neyret, "Precomputed Atmospheric
// Scattering" (EGSR 2008), evaluated with Hillaire's LUTs. Static: computed
// once with the transmittance and multiple-scattering LUTs. RGBA32F because
// twilight values span many decades below the noon sky.

import atmosphere;

[[vk::binding(0)]] Texture2D           g_transmittance;
[[vk::binding(1)]] Texture2D           g_multiscatter;
[[vk::binding(2)]] SamplerState        g_samp;
[[vk::binding(3)]] [format("rgba32f")] RWTexture2D<float4> g_out;

static const int ZENITH_STEPS = 16;
static const int AZIMUTH_STEPS = 32;
static const int MARCH_STEPS = 24;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= SKY_IRRADIANCE_TEXELS) return;
    float mu_light = sky_irradiance_mu_from_texel(tid.x);
    float3 light = float3(sqrt(saturate(1.0 - mu_light * mu_light)), mu_light, 0.0);
    // Just above the ground: the patch the irradiance lands on.
    float r = ATMOS_GROUND_RADIUS + 1e-3;

    float d_theta = 0.5 * ATMOS_PI / float(ZENITH_STEPS);
    float d_phi = 2.0 * ATMOS_PI / float(AZIMUTH_STEPS);
    float3 irradiance = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < ZENITH_STEPS; ++i) {
        float theta = (float(i) + 0.5) * d_theta;
        float mu = cos(theta);
        float sin_theta = sin(theta);
        float length = distance_to_top(r, mu);
        for (int j = 0; j < AZIMUTH_STEPS; ++j) {
            float phi = (float(j) + 0.5) * d_phi;
            float3 dir = float3(sin_theta * cos(phi), mu, sin_theta * sin(phi));
            float3 throughput;
            float3 l = integrate_inscattering(g_transmittance, g_multiscatter, g_samp, r, mu,
                                              mu_light, dot(dir, light), length, MARCH_STEPS,
                                              throughput);
            irradiance += l * mu * sin_theta * d_theta * d_phi;
        }
    }
    g_out[int2(int(tid.x), 0)] = float4(irradiance, 1.0);
}
