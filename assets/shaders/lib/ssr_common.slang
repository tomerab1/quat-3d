// ssr_common.slang — shared parts of the screen-space reflection passes
// (ROADMAP R21; SsrPass, ssr_trace / ssr_resolve / ssr_temporal.slang).
//
// Stochastic SSR (Stachowiak 2015, "Stochastic Screen-Space Reflections"):
// one GGX visible-normal sample per pixel (Heitz 2018) traced through the
// Hi-Z pyramid's nearest-depth channel (Uludag 2014, "Hi-Z Screen-Space
// Cone-Traced Reflections"; the traversal follows AMD FidelityFX SSSR's
// min-Z march), resolved with ray reuse between neighbours weighted by the
// BRDF / pdf ratio, then accumulated temporally.

// Per-frame values (std140, 192 bytes; SsrConstants in ssr_pass.cpp).
// Matrices are glm columns (applied with ssr_mul).
public struct SsrConstants {
    public float4 view_proj[4];     // world -> clip (jittered, as the depth buffer)
    public float4 inv_view_proj[4]; // clip -> world
    public float4 camera_pos;       // xyz world, w frame index (noise)
    public float4 screen;           // xy extent, zw 1 / extent
    public float4 params;           // x max roughness, y thickness m, z edge fade, w max steps
    public float4 params2;          // x Hi-Z mip count, y colour valid, z feedback, w reuse samples
}

public static const float SSR_PI = 3.14159265358979;

public float4 ssr_mul(float4 cols[4], float4 v) {
    return cols[0] * v.x + cols[1] * v.y + cols[2] * v.z + cols[3] * v.w;
}

// Screen uv + NDC depth -> world position.
public float3 ssr_world(float4 inv_view_proj[4], float2 uv, float depth) {
    float4 w = ssr_mul(inv_view_proj, float4(uv * 2.0 - 1.0, depth, 1.0));
    return w.xyz / w.w;
}

// World position -> (screen uv, NDC depth); w <= 0 behind the camera.
public float4 ssr_project(float4 view_proj[4], float3 p) {
    float4 c = ssr_mul(view_proj, float4(p, 1.0));
    return float4(c.xy / c.w * 0.5 + 0.5, c.z / c.w, c.w);
}

// Interleaved gradient noise (Jimenez 2014), rotated per frame.
public float ssr_noise(float2 pixel, float frame) {
    float2 p = pixel + 5.588238 * fmod(frame, 64.0);
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Two decorrelated uniform numbers for `pixel` on `frame` (R2-rotated IGN).
public float2 ssr_random2(float2 pixel, float frame) {
    float a = ssr_noise(pixel, frame);
    float b = ssr_noise(pixel + float2(37.0, 17.0), frame);
    return frac(float2(a, b) + frame * float2(0.7548776662, 0.5698402910));
}

// GGX D with alpha = roughness^2.
public float ssr_d_ggx(float ndoth, float a) {
    float a2 = a * a;
    float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
    return a2 / (SSR_PI * d * d);
}

// Smith G1 for GGX.
public float ssr_g1(float ndotv, float a) {
    float a2 = a * a;
    return 2.0 * ndotv / (ndotv + sqrt(a2 + (1.0 - a2) * ndotv * ndotv));
}

// Height-correlated Smith visibility (pbr.slang's).
public float ssr_vis(float ndotv, float ndotl, float a) {
    float a2 = a * a;
    float gv = ndotl * sqrt(ndotv * ndotv * (1.0 - a2) + a2);
    float gl = ndotv * sqrt(ndotl * ndotl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// pdf of a reflected direction drawn from the visible normals:
// G1(V) D(H) / (4 NdotV).
public float ssr_vndf_pdf(float ndotv, float ndoth, float a) {
    return ssr_g1(ndotv, a) * ssr_d_ggx(ndoth, a) / (4.0 * max(ndotv, 1e-4));
}

// Heitz 2018: a GGX visible normal for view `ve` in tangent space (z up).
public float3 ssr_sample_vndf(float3 ve, float a, float2 u) {
    float3 vh = normalize(float3(a * ve.x, a * ve.y, ve.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    float3 t1 = lensq > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 t2 = cross(vh, t1);
    float r = sqrt(u.x);
    float phi = 2.0 * SSR_PI * u.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;
    float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    return normalize(float3(a * nh.x, a * nh.y, max(nh.z, 1e-6)));
}

// Orthonormal basis around n (Duff et al. 2017).
public void ssr_basis(float3 n, out float3 t, out float3 b) {
    float sign_z = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign_z + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + sign_z * n.x * n.x * a, sign_z * c, -sign_z * n.x);
    b = float3(c, sign_z + n.y * n.y * a, -n.y);
}

// Roughness fade: full weight below 75% of the cutoff, none above it.
public float ssr_roughness_fade(float roughness, float max_roughness) {
    return 1.0 - smoothstep(max_roughness * 0.75, max_roughness, roughness);
}
