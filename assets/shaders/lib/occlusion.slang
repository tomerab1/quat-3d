// occlusion.slang — applying screen-space occlusion in the lighting pass
// (ROADMAP R12): GTAO's multi-bounce approximation and bent-cone specular
// occlusion, and the contact-shadow ray march.
//
// References:
//   * Jimenez, Wu, Pesce, Jarabo, "Practical Realtime Strategies for
//     Accurate Indirect Occlusion", SIGGRAPH 2016 (multi-bounce fit, specular
//     occlusion as a visibility-cone / specular-cone intersection).
//   * Oat, Sander, "Ambient Aperture Lighting", I3D 2007 (spherical-cap
//     intersection solid angle, smoothstep approximation).
//   * Contact shadows: a short screen-space march toward the light through
//     the depth buffer (as in UE4/5 and Bend Studio's screen-space shadows).

static const float OCC_PI = 3.14159265358979;

// Albedo-aware multi-bounce AO (Jimenez 2016, eq. 10): brightens occlusion
// on light surfaces, where interreflections fill the crevices.
public float3 gtao_multi_bounce(float visibility, float3 albedo) {
    float3 a = 2.0404 * albedo - 0.3324;
    float3 b = -4.7951 * albedo + 0.6417;
    float3 c = 2.7552 * albedo + 0.6903;
    float x = visibility;
    return max(float3(x, x, x), ((x * a + b) * x + c) * x);
}

// Solid angle of the intersection of two spherical caps with cosines of
// half-angle cos_a / cos_b whose axes are cos_d apart.
float cap_intersection(float cos_a, float cos_b, float cos_d) {
    float ra = acos(clamp(cos_a, -1.0, 1.0));
    float rb = acos(clamp(cos_b, -1.0, 1.0));
    float d = acos(clamp(cos_d, -1.0, 1.0));
    float smaller_cap = 2.0 * OCC_PI - 2.0 * OCC_PI * max(cos_a, cos_b);
    if (min(ra, rb) <= max(ra, rb) - d) {
        return smaller_cap; // one cap inside the other
    }
    if (ra + rb <= d) {
        return 0.0;
    }
    float delta = abs(ra - rb);
    float x = 1.0 - saturate((d - delta) / max(ra + rb - delta, 1e-4));
    return smaller_cap * x * x * (3.0 - 2.0 * x);
}

// Fraction of the specular lobe around reflection `r` (roughness `rough`)
// that the visibility cone (axis = bent normal, aperture from the GTAO
// visibility) leaves open.
public float gtao_specular_occlusion(float3 bent_normal, float visibility, float3 r, float rough) {
    float cos_vis = sqrt(saturate(1.0 - visibility));
    float cos_spec = exp2(-3.32193 * rough * rough);
    float spec_cap = 2.0 * OCC_PI * (1.0 - cos_spec);
    return saturate(cap_intersection(cos_vis, cos_spec, dot(bent_normal, r)) / max(spec_cap, 1e-4));
}

// Contact shadow: 1 = lit, 0 = a short ray toward the sun hits the depth
// buffer. `clip0` / `clip1` are the ray's start and end in clip space
// (perspective: clip.w is the view distance, compared against the depth
// buffer linearised with depth_lin = (A, B): view distance = B / (d + A)).
public float contact_shadow(Texture2D depth, uint2 size, float4 clip0, float4 clip1,
                            float2 depth_lin, uint steps, float thickness, float noise) {
    for (uint i = 0; i < steps; ++i) {
        float t = (float(i) + noise) / float(steps);
        float4 c = lerp(clip0, clip1, t);
        if (c.w <= 0.0) {
            break;
        }
        float2 uv = (c.xy / c.w) * 0.5 + 0.5;
        if (any(uv <= 0.0) || any(uv >= 1.0)) {
            break;
        }
        float d = depth.Load(int3(int2(uv * float2(size)), 0)).r;
        if (d >= 1.0) {
            continue;
        }
        float scene = depth_lin.y / (d + depth_lin.x);
        float delta = c.w - scene;
        if (delta > c.w * 0.002 && delta < thickness) {
            return 0.0;
        }
    }
    return 1.0;
}
