// ibl_bake_env.slang — bake the procedural sky into an environment cubemap
// (Phase 7.6). One thread per cube texel; writes sky(dir) for that texel's world
// direction. The result feeds the irradiance + prefilter convolutions.

import sky;
import ibl_common;

struct BakeParams {
    float4 sun_dir;  // xyz = direction to the sun
    uint   face_size; // pixels per cube face edge
    uint   pad0;
    uint   pad1;
    uint   pad2;
}
[[vk::push_constant]] BakeParams pc;

[[vk::binding(0)]] RWTexture2DArray<float4> g_out; // 6-layer cube as a 2D array

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.face_size || tid.y >= pc.face_size) {
        return;
    }
    float2 uv = (float2(tid.xy) + 0.5) / float(pc.face_size);
    float3 dir = cube_dir(tid.z, uv);
    g_out[tid] = float4(sky(dir, normalize(pc.sun_dir.xyz)), 1.0);
}
