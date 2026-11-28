// brdf_energy.slang — energy-conserving specular (ROADMAP R21).
//
// A single-scattering GGX lobe loses the energy of light that bounces more
// than once between microfacets: rough metals render visibly dark (a white
// furnace of roughness 1 keeps ~60%). Both terms here restore it from the
// split-sum DFG LUT (ibl_brdf_lut.slang: x = A, y = B with
// E_ss(mu) = F0 * A + B, A + B = the directional albedo for F = 1):
//
//   * Image-based lighting: Fdez-Agüera 2019, "A Multiple-Scattering
//     Microfacet Model for Real-Time Image-based Lighting" (JCGT 8.1) — the
//     single-scatter split sum, plus a multiple-scatter lobe lit by the
//     cosine-weighted irradiance, plus the diffuse energy the specular lobes
//     leave (so a dielectric's diffuse is not lit twice).
//   * Punctual lights: the Kulla-Conty 2017 compensation in Filament's form —
//     the single-scatter lobe scaled by 1 + F0 (1 / E_ss(1) - 1).
//
// With `enabled` false both reduce to the classic single-scatter terms
// (kD = (1 - F) (1 - metallic)), which keeps the A8 toggle a straight A/B.

// Weights of one pixel's image-based light. Every term multiplies one input:
//   lit = single * prefiltered_radiance + (multi + diffuse) * irradiance
// where irradiance is the irradiance cube's value (E / pi convention: the
// cosine-weighted average radiance).
public struct IblEnergy {
    public float3 single;  // single-scatter specular (F_ss E_ss)
    public float3 multi;   // multiple-scatter specular (F_ms E_ms)
    public float3 diffuse; // diffuse colour times the energy left to it
}

// `f_rough` is the roughness-aware Schlick Fresnel at NdotV (Fdez-Agüera's
// k_S), `dfg` the LUT's (A, B) at (NdotV, roughness) and `diffuse_color` the
// Lambert albedo (base colour * (1 - metallic)).
public IblEnergy ibl_energy(float3 f0, float3 f_rough, float2 dfg, float3 diffuse_color,
                            bool enabled) {
    IblEnergy e;
    e.single = f_rough * dfg.x + dfg.y;
    if (!enabled) {
        e.multi = float3(0.0, 0.0, 0.0);
        e.diffuse = diffuse_color * (1.0 - f_rough);
        return e;
    }
    float  ess = dfg.x + dfg.y;
    float  ems = 1.0 - ess;
    float3 f_avg = f0 + (1.0 - f0) / 21.0;
    float3 f_ms = e.single * f_avg / (1.0 - ems * f_avg);
    e.multi = f_ms * ems;
    e.diffuse = diffuse_color * (1.0 - (e.single + e.multi));
    return e;
}

// Multiplier on a punctual light's single-scatter specular lobe that adds
// the missing multiple-scatter energy (1 when disabled).
public float3 punctual_energy_compensation(float3 f0, float2 dfg, bool enabled) {
    if (!enabled) {
        return float3(1.0, 1.0, 1.0);
    }
    float ess = max(dfg.x + dfg.y, 1e-3);
    return 1.0 + f0 * (1.0 / ess - 1.0);
}

// Horizon occlusion (Jimenez et al. 2016, after Russell's "Horizon
// Occlusion for Normal Mapped Reflections"): a normal-mapped surface can
// reflect directions below its geometric surface, which the environment
// would light although the surface itself blocks them. Fades the specular
// reflection as `r` dips under the plane of the geometric normal `ng`;
// `fade` = 1.3 is the shipped constant.
public float horizon_occlusion(float3 r, float3 ng) {
    float h = saturate(1.0 + 1.3 * dot(r, ng));
    return h * h;
}
