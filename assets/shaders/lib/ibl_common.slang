// ibl_common.slang — shared math for the split-sum IBL precompute (Phase 7.6).
//
// Cubemap face<->direction mapping (Vulkan/D3D convention, so writes via
// cube_dir() land where TextureCube sampling reads them back), the Hammersley
// low-discrepancy sequence, and GGX importance sampling. Used by the env-bake,
// irradiance, prefilter, and BRDF-LUT compute shaders.

public static const float IBL_PI = 3.14159265358979;

// World-space direction for cube face `face` (0=+X,1=-X,2=+Y,3=-Y,4=+Z,5=-Z) at
// face uv in [0,1] (v downward). Matches the standard hardware cubemap layout.
public float3 cube_dir(uint face, float2 uv) {
    float s = 2.0 * uv.x - 1.0;
    float t = 2.0 * uv.y - 1.0;
    float3 d;
    switch (face) {
        case 0: d = float3( 1.0,   -t,   -s); break; // +X
        case 1: d = float3(-1.0,   -t,    s); break; // -X
        case 2: d = float3(  s,   1.0,    t); break; // +Y
        case 3: d = float3(  s,  -1.0,   -t); break; // -Y
        case 4: d = float3(  s,    -t,  1.0); break; // +Z
        default: d = float3(-s,   -t, -1.0); break;  // -Z
    }
    return normalize(d);
}

// Van der Corput radical inverse (base 2) via bit reversal.
public float radical_inverse_vdc(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 2^32
}

// i-th of N points on the Hammersley 2D sequence.
public float2 hammersley(uint i, uint n) {
    return float2(float(i) / float(n), radical_inverse_vdc(i));
}

// GGX importance-sampled half-vector around normal N, for roughness `rough`.
public float3 importance_sample_ggx(float2 xi, float3 n, float rough) {
    float a = rough * rough;
    float phi = 2.0 * IBL_PI * xi.x;
    float cos_t = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_t = sqrt(1.0 - cos_t * cos_t);

    float3 h = float3(cos(phi) * sin_t, sin(phi) * sin_t, cos_t);

    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, n));
    float3 bitangent = cross(n, tangent);
    return normalize(tangent * h.x + bitangent * h.y + n * h.z);
}

// GGX NDF (for the prefilter mip/pdf weighting).
public float ibl_d_ggx(float ndoth, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
    return a2 / (IBL_PI * d * d);
}

// Smith geometry term (IBL k = a^2 / 2) for the BRDF-LUT integration.
public float ibl_g_smith(float ndotv, float ndotl, float rough) {
    float a = rough * rough;
    float k = a * a * 0.5;
    float gv = ndotv / (ndotv * (1.0 - k) + k);
    float gl = ndotl / (ndotl * (1.0 - k) + k);
    return gv * gl;
}
