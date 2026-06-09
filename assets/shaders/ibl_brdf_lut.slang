// ibl_brdf_lut.slang — split-sum BRDF integration LUT (Phase 7.6).
//
// Output.x = (NdotV, roughness) -> scale, Output.y = bias for the environment
// BRDF: specular = prefiltered · (F0 · scale + bias). Independent of the
// environment, so it is baked once. Written to an RGBA16F image (rg used).

import ibl_common;

struct LutParams {
    uint size;
    uint pad0;
    uint pad1;
    uint pad2;
}
[[vk::push_constant]] LutParams pc;

[[vk::binding(0)]] RWTexture2D<float4> g_out;

static const uint LUT_SAMPLES = 1024u;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.size || tid.y >= pc.size) {
        return;
    }
    float ndotv = (float(tid.x) + 0.5) / float(pc.size);
    float roughness = (float(tid.y) + 0.5) / float(pc.size);
    ndotv = max(ndotv, 1e-3);

    float3 V = float3(sqrt(1.0 - ndotv * ndotv), 0.0, ndotv);
    float3 N = float3(0.0, 0.0, 1.0);

    float a = 0.0;
    float b = 0.0;
    for (uint i = 0; i < LUT_SAMPLES; ++i) {
        float2 xi = hammersley(i, LUT_SAMPLES);
        float3 H = importance_sample_ggx(xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);
        float ndotl = max(L.z, 0.0);
        float ndoth = max(H.z, 0.0);
        float vdoth = max(dot(V, H), 0.0);
        if (ndotl > 0.0) {
            float g = ibl_g_smith(ndotv, ndotl, roughness);
            float g_vis = g * vdoth / (ndoth * ndotv);
            float fc = pow(1.0 - vdoth, 5.0);
            a += (1.0 - fc) * g_vis;
            b += fc * g_vis;
        }
    }
    g_out[tid.xy] = float4(a / float(LUT_SAMPLES), b / float(LUT_SAMPLES), 0.0, 1.0);
}
