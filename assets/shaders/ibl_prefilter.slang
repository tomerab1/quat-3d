// ibl_prefilter.slang — prefiltered specular environment (Phase 7.6).
//
// Runs once per mip of the specular cube; `roughness` increases with mip. Each
// output texel (a reflection direction R, assuming N = V = R) is the GGX
// importance-sampled average of the environment, i.e. the preintegrated specular
// radiance the lighting pass reads at lod = roughness · maxLod.

import ibl_common;

struct PrefilterParams {
    float roughness;
    uint  mip_size;
    uint  pad0;
    uint  pad1;
}
[[vk::push_constant]] PrefilterParams pc;

[[vk::binding(0)]] TextureCube              g_env;
[[vk::binding(1)]] SamplerState             g_samp;
[[vk::binding(2)]] RWTexture2DArray<float4> g_out;

static const uint PREFILTER_SAMPLES = 128u;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.mip_size || tid.y >= pc.mip_size) {
        return;
    }
    float2 uv = (float2(tid.xy) + 0.5) / float(pc.mip_size);
    float3 N = cube_dir(tid.z, uv);
    float3 V = N;

    // Mip 0 (roughness 0) is a straight mirror copy — skip the integration.
    if (pc.roughness <= 0.0) {
        g_out[tid] = float4(g_env.SampleLevel(g_samp, N, 0.0).rgb, 1.0);
        return;
    }

    float3 color = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    for (uint i = 0; i < PREFILTER_SAMPLES; ++i) {
        float2 xi = hammersley(i, PREFILTER_SAMPLES);
        float3 H = importance_sample_ggx(xi, N, pc.roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);
        float ndotl = dot(N, L);
        if (ndotl > 0.0) {
            color += g_env.SampleLevel(g_samp, L, 0.0).rgb * ndotl;
            total += ndotl;
        }
    }
    color = total > 0.0 ? color / total : g_env.SampleLevel(g_samp, N, 0.0).rgb;
    g_out[tid] = float4(color, 1.0);
}
