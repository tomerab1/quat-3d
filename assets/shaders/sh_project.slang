// sh_project.slang — projects an environment cube onto SH9 (ROADMAP R21).
//
// One workgroup per cube face (dispatch 6 x 1 x 1) reads a small mip of the
// environment (Load, every texel weighted by its exact solid angle) and
// reduces its 64 threads' partial sums in shared memory. Face f writes
// g_sh[f * 10 + k] = sum of L * Y_k * dw (k < 9) and g_sh[f * 10 + 9].x =
// the face's total solid angle; sh_irradiance.slang combines the faces.

import ibl_common; // cube_dir
import sh9;

struct ShProjectParams {
    uint size; // face edge of the mip read
    uint pad0;
    uint pad1;
    uint pad2;
}
[[vk::push_constant]] ShProjectParams pc;

Texture2DArray<float4>     g_src;
RWStructuredBuffer<float4> g_sh;

static const uint SH_THREADS = 64u;
groupshared float3 gs_sum[SH_THREADS][9];
groupshared float  gs_weight[SH_THREADS];

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 group : SV_GroupID, uint thread : SV_GroupIndex) {
    uint face = group.x;
    float3 sum[9];
    for (uint k = 0; k < SH9_COUNT; ++k) {
        sum[k] = float3(0.0, 0.0, 0.0);
    }
    float weight = 0.0;
    uint count = pc.size * pc.size;
    for (uint i = thread; i < count; i += SH_THREADS) {
        uint2 texel = uint2(i % pc.size, i / pc.size);
        float2 uv = (float2(texel) + 0.5) / float(pc.size);
        float3 d = cube_dir(face, uv);
        float dw = cube_texel_solid_angle(texel, pc.size);
        float3 radiance = g_src.Load(int4(int2(texel), int(face), 0)).rgb;
        float b[9];
        sh9_basis(d, b);
        for (uint k = 0; k < SH9_COUNT; ++k) {
            sum[k] += radiance * (b[k] * dw);
        }
        weight += dw;
    }
    for (uint k = 0; k < SH9_COUNT; ++k) {
        gs_sum[thread][k] = sum[k];
    }
    gs_weight[thread] = weight;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = SH_THREADS / 2u; stride > 0u; stride >>= 1u) {
        if (thread < stride) {
            for (uint k = 0; k < SH9_COUNT; ++k) {
                gs_sum[thread][k] += gs_sum[thread + stride][k];
            }
            gs_weight[thread] += gs_weight[thread + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread == 0u) {
        for (uint k = 0; k < SH9_COUNT; ++k) {
            g_sh[face * 10u + k] = float4(gs_sum[0][k], 0.0);
        }
        g_sh[face * 10u + 9u] = float4(gs_weight[0], 0.0, 0.0, 0.0);
    }
}
