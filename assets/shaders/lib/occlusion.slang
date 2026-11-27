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
//   * Y. Wu, "Accurate Normal Reconstruction from Depth Buffer", 2021
//     (choosing the depth-buffer neighbour on a texel's own surface by
//     comparing the second differences on either side).

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

// Depth texel `p`, background (1) outside the image.
float depth_texel(Texture2D depth, int2 p, int2 last) {
    if (any(p < 0) || any(p > last)) {
        return 1.0;
    }
    return depth.Load(int3(p, 0)).r;
}

// Slope of the depth buffer at `texel` along `axis` (depth units per texel),
// given its neighbours `dm` (texel - axis) and `dp` (texel + axis). NDC depth
// is affine in screen space over a plane, so through a planar footprint the
// two one-sided differences agree (second difference within `tol`) and the
// central difference is exact. Otherwise a depth edge or a curve runs beside
// the texel: keep the side whose next texel continues the same surface, the
// one-sided difference with the smaller second difference on its own side
// (as in Y. Wu's depth-buffer normal reconstruction), so the slope never spans
// the edge. A texel that neither side continues (a feature one
// pixel thin) gets slope 0: its own depth, never a ramp toward what is
// behind it.
float depth_slope(Texture2D depth, int2 texel, int2 axis, int2 last, float dm, float d0,
                  float dp, float tol) {
    bool vm = all(texel - axis >= 0) && dm < 1.0;
    bool vp = all(texel + axis <= last) && dp < 1.0;
    float back = d0 - dm;
    float fwd = dp - d0;
    if (vm && vp && abs(fwd - back) <= tol) {
        return 0.5 * (back + fwd);
    }
    float back_curve = 1e30;
    float fwd_curve = 1e30;
    if (vm) {
        float dmm = depth_texel(depth, texel - 2 * axis, last);
        if (dmm < 1.0) {
            back_curve = abs(back - (dm - dmm));
        }
    }
    if (vp) {
        float dpp = depth_texel(depth, texel + 2 * axis, last);
        if (dpp < 1.0) {
            fwd_curve = abs((dpp - dp) - fwd);
        }
    }
    float slope = back_curve <= fwd_curve ? back : fwd;
    float curve = min(back_curve, fwd_curve);
    return curve <= max(tol, 0.5 * abs(slope)) ? slope : 0.0;
}

// Contact shadow: 1 = lit, 0 = a short ray toward the sun hits the depth
// buffer. `clip0` / `clip1` are the ray's start and end in clip space
// (perspective: clip.w is the view distance, compared against the depth
// buffer linearised with depth_lin = (A, B): view distance = B / (d + A)).
//
// The scene depth at each sample is the nearest texel's depth carried along
// that texel's own surface (depth_slope per axis) to the sample's sub-pixel
// position. NDC depth is affine in screen space over a plane, so a planar
// receiver is reconstructed exactly and cannot shadow itself, even seen at a
// grazing angle where one pixel spans metres of depth. Interpolating between
// the 2x2 nearest texels instead is exact only on planes: across a depth
// discontinuity (a sphere's silhouette in front of a floor) it invents a
// surface in between, which floor rays crossing the silhouette hit in a
// dotted halo (#86). The planarity tolerance is relative to d + A = B / w,
// the reciprocal view distance: 1e-3 of it is a depth step of 0.1 % of the
// distance, the same as the self-hit guard below, plus ~16 ulps of a D32
// depth near 1.
public float contact_shadow(Texture2D depth, SamplerState point_sampler, uint2 size,
                            float4 clip0, float4 clip1, float2 depth_lin, uint steps,
                            float thickness, float noise) {
    float2 fsize = float2(size);
    int2 last = int2(size) - 1;
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
        float2 p = uv * fsize;
        int2 texel = min(int2(p), last);
        float2 offset = p - (float2(texel) + 0.5); // [-0.5, 0.5] texels
        // Gather order: x = (0,1), y = (1,1), z = (1,0), w = (0,0) of the 2x2;
        // `lo` covers texel-1 .. texel, `hi` covers texel .. texel+1.
        float4 lo = depth.GatherRed(point_sampler, float2(texel) / fsize);
        float4 hi = depth.GatherRed(point_sampler, float2(texel + 1) / fsize);
        float d0 = lo.y;
        if (d0 >= 1.0) {
            continue;
        }
        float tol = 1e-3 * abs(d0 + depth_lin.x) + 1e-6;
        float sx = depth_slope(depth, texel, int2(1, 0), last, lo.x, d0, hi.z, tol);
        float sy = depth_slope(depth, texel, int2(0, 1), last, lo.z, d0, hi.x, tol);
        float d = d0 + offset.x * sx + offset.y * sy;
        if (d >= 1.0) {
            continue;
        }
        float scene = depth_lin.y / (d + depth_lin.x);
        float delta = c.w - scene;
        if (delta > c.w * 0.001 && delta < thickness) {
            return 0.0;
        }
    }
    return 1.0;
}
