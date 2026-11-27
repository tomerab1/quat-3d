// sh_irradiance.slang — irradiance cube from the SH9 projection (R21).
//
// Sums sh_project.slang's six per-face partials (normalised so the texel
// solid angles total exactly 4 pi), convolves with the clamped cosine and
// evaluates E / pi per texel of the small irradiance cube the lighting pass
// samples with the (bent) normal. Negative lobes (ringing around a very
// bright, unremoved sun) are clamped to zero. Texel (0, 0) of face 0 also
// stores the final coefficients at g_sh[60 + k] (tests, CPU readback).

import ibl_common; // cube_dir
import sh9;

struct ShIrradianceParams {
    uint size; // irradiance cube face edge
    uint pad0;
    uint pad1;
    uint pad2;
}
[[vk::push_constant]] ShIrradianceParams pc;

RWStructuredBuffer<float4>                   g_sh;
[format("rgba16f")] RWTexture2DArray<float4> g_out;

static const uint SH_FINAL = 60u;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.size || tid.y >= pc.size) {
        return;
    }
    float3 coeffs[9];
    float weight = 0.0;
    for (uint k = 0; k < SH9_COUNT; ++k) {
        coeffs[k] = float3(0.0, 0.0, 0.0);
    }
    for (uint f = 0; f < 6u; ++f) {
        for (uint k = 0; k < SH9_COUNT; ++k) {
            coeffs[k] += g_sh[f * 10u + k].rgb;
        }
        weight += g_sh[f * 10u + 9u].x;
    }
    float norm = weight > 0.0 ? 4.0 * SH_PI / weight : 0.0;

    float2 uv = (float2(tid.xy) + 0.5) / float(pc.size);
    float3 n = cube_dir(tid.z, uv);
    float b[9];
    sh9_basis(n, b);
    float3 e = float3(0.0, 0.0, 0.0);
    for (uint k = 0; k < SH9_COUNT; ++k) {
        e += coeffs[k] * norm * (sh9_band_weight(k) * b[k]);
    }
    g_out[tid] = float4(max(e, float3(0.0, 0.0, 0.0)), 1.0);
    if (all(tid == uint3(0u, 0u, 0u))) {
        for (uint k = 0; k < SH9_COUNT; ++k) {
            g_sh[SH_FINAL + k] = float4(coeffs[k] * norm, 0.0);
        }
    }
}
