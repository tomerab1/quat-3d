// atmos_aerial.slang — fills the aerial perspective volume (ROADMAP
// R13.3, AerialPerspectivePass; Hillaire 2020 sec. 5.3, see
// lib/aerial_perspective.slang). One thread per screen texel column: march
// the view ray through the AP_SLICES depth slices (two sub-steps each),
// accumulating single scattering of each light through the transmittance
// LUT plus the multiple-scattering LUT's isotropic term, integrated
// analytically per step (Hillaire 2015's energy-conserving form), and
// write every slice on the way.

import atmosphere;
import aerial_perspective;

struct ApPush {
    float4 inv_view_proj[4]; // clip -> world (unjittered), glm columns
    float4 sun;              // xyz unit toward the sun, w extra-atmospheric irradiance (rel.)
    float4 moon;             // xyz unit toward the moon, w irradiance (rel., 0 = none)
}
[[vk::push_constant]] ApPush pc;

Texture2D                            g_transmittance;
Texture2D                            g_multiscatter;
SamplerState                         g_sampler;
[format("rgba16f")] RWTexture3D<float4> g_ap;

float3 view_ray(float2 uv) {
    float2 ndc = uv * 2.0 - 1.0;
    float4 a = pc.inv_view_proj[0] * ndc.x + pc.inv_view_proj[1] * ndc.y +
               pc.inv_view_proj[2] * 1.0 + pc.inv_view_proj[3];
    float4 b = pc.inv_view_proj[0] * ndc.x + pc.inv_view_proj[1] * ndc.y +
               pc.inv_view_proj[2] * 0.5 + pc.inv_view_proj[3];
    return normalize(a.xyz / a.w - b.xyz / b.w);
}

// In-scattered radiance per unit length at planet-centred point `pos` (km)
// from one light.
float3 light_scattering(float3 pos, float4 light, float cos_theta, MediumSample m) {
    if (light.w <= 0.0) return float3(0.0, 0.0, 0.0);
    float r = length(pos);
    float mu = dot(pos, light.xyz) / r;
    float3 trans = intersects_ground(r, mu) ? float3(0.0, 0.0, 0.0)
                                            : sample_transmittance(g_transmittance, g_sampler, r, mu);
    float3 multi = sample_multiscatter(g_multiscatter, g_sampler, r, mu);
    float3 phased = m.rayleigh_scatter * rayleigh_phase(cos_theta) +
                    m.mie_scatter.xxx * mie_phase(cos_theta);
    float3 total = m.rayleigh_scatter + m.mie_scatter.xxx;
    return light.w * (phased * trans + total * multi);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= AP_SIZE || tid.y >= AP_SIZE) return;
    float2 uv = (float2(tid.xy) + 0.5) / float(AP_SIZE);
    float3 dir = view_ray(uv);
    float3 origin = float3(0.0, ATMOS_VIEW_HEIGHT, 0.0);
    float cos_sun = dot(dir, pc.sun.xyz);
    float cos_moon = dot(dir, pc.moon.xyz);
    float3 lum = float3(0.0, 0.0, 0.0);
    float3 trans = float3(1.0, 1.0, 1.0);
    float t = 0.0;
    for (uint k = 0; k < AP_SLICES; ++k) {
        float t_end = ap_depth_km((float(k) + 0.5) / float(AP_SLICES));
        float dt = (t_end - t) * 0.5;
        for (int s = 0; s < 2; ++s) {
            float3 pos = origin + dir * (t + dt * 0.5);
            float r = clamp(length(pos), ATMOS_GROUND_RADIUS + 1e-4, ATMOS_TOP_RADIUS);
            MediumSample m = sample_medium(r - ATMOS_GROUND_RADIUS);
            float3 sc = light_scattering(pos, pc.sun, cos_sun, m) +
                        light_scattering(pos, pc.moon, cos_moon, m);
            float3 step_trans = exp(-m.extinction * dt);
            lum += trans * (sc - sc * step_trans) / max(m.extinction, 1e-6);
            trans *= step_trans;
            t += dt;
        }
        g_ap[uint3(tid.xy, k)] = float4(lum, dot(trans, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)));
    }
}
