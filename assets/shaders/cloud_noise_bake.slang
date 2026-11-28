// cloud_noise_bake.slang — bakes the clouds' 3D noise volumes (ROADMAP R13.4,
// CloudNoisePass). One thread per texel, evaluated at the texel centre:
// volume 0 is the 128³ base (Perlin-Worley + Worley fBm), volume 1 the 32³
// detail (Worley fBm); see lib/cloud_noise.slang. Run once at startup.

import cloud_noise;

struct BakePush {
    uint volume; // 0 base, 1 detail
    uint size;   // edge in texels
    uint pad0;
    uint pad1;
}
[[vk::push_constant]] BakePush pc;

[format("rgba8")] RWTexture3D<float4> g_volume;

[shader("compute")]
[numthreads(4, 4, 4)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (any(tid >= uint3(pc.size))) return;
    float3 uvw = (float3(tid) + 0.5) / float(pc.size);
    g_volume[tid] = pc.volume == 0u ? cloud_base_noise(uvw) : cloud_detail_noise(uvw);
}
