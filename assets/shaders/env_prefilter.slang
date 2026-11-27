// env_prefilter.slang — prefiltered specular cube for an HDRI environment
// (ROADMAP R21): the split-sum's first factor (Karis 2013) with filtered
// importance sampling (Křivánek & Colbert 2008, GPU Gems 3 ch. 20).
//
// One dispatch per output mip; roughness grows linearly with the mip (the
// lighting pass reads lod = roughness * PREFILTER_MAX_LOD). Each texel is a
// reflection direction R with N = V = R. GGX-distributed half vectors give
// light directions L; instead of point-sampling the environment (which
// needs thousands of samples before an HDRI's bright sun stops sparkling),
// each sample reads the environment's mip chain at the level whose texel
// matches the sample's solid angle, Omega_s = 1 / (N pdf), against a mip-0
// texel's Omega_p = 4 pi / (6 size^2): lod = 0.5 log2(Omega_s / Omega_p). (GPU Gems
// adds a +1 bias; at high roughness that blurs over whole faces and biases
// the result next to kinks such as a horizon, so it is left out: 128
// samples keep the estimate noise-free without it.)
// With N = V the GGX pdf of L reduces to D / 4.

import ibl_common;
import hdr;

struct EnvPrefilterParams {
    float roughness;
    uint  mip_size; // texels per face edge of the mip being written
    float env_size; // texels per face edge of the environment's mip 0
    uint  samples;
}
[[vk::push_constant]] EnvPrefilterParams pc;

TextureCube                                  g_env; // every mip
SamplerState                                 g_env_sampler;
[format("rgba16f")] RWTexture2DArray<float4> g_out;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.mip_size || tid.y >= pc.mip_size) {
        return;
    }
    float2 uv = (float2(tid.xy) + 0.5) / float(pc.mip_size);
    float3 N = cube_dir(tid.z, uv);

    // The mirror level reads the environment at the footprint of one output
    // texel (the output base may be smaller than the environment).
    float footprint_lod = log2(max(pc.env_size / float(pc.mip_size), 1.0));
    if (pc.roughness <= 0.0) {
        g_out[tid] = float4(g_env.SampleLevel(g_env_sampler, N, footprint_lod).rgb, 1.0);
        return;
    }

    float omega_p = 4.0 * IBL_PI / (6.0 * pc.env_size * pc.env_size);
    float3 color = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    for (uint i = 0; i < pc.samples; ++i) {
        float2 xi = hammersley(i, pc.samples);
        float3 H = importance_sample_ggx(xi, N, pc.roughness);
        float ndoth = saturate(dot(N, H));
        float3 L = normalize(2.0 * ndoth * H - N);
        float ndotl = dot(N, L);
        if (ndotl > 0.0) {
            float pdf = ibl_d_ggx(ndoth, pc.roughness) * 0.25;
            float omega_s = 1.0 / (float(pc.samples) * max(pdf, 1e-6));
            float lod = max(0.5 * log2(omega_s / omega_p), footprint_lod);
            color += g_env.SampleLevel(g_env_sampler, L, lod).rgb * ndotl;
            total += ndotl;
        }
    }
    color = total > 0.0 ? color / total : g_env.SampleLevel(g_env_sampler, N, footprint_lod).rgb;
    g_out[tid] = float4(hdr_store(color), 1.0);
}
