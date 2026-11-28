// cube_downsample.slang — one mip of an environment cube's chain (R21):
// each texel is the box average of its 2x2 footprint in the level above, on
// all six faces (z = face). The chain feeds filtered importance sampling
// (env_prefilter.slang) and the SH9 projection (sh_project.slang).

struct DownsampleParams {
    uint dst_size;
    uint pad0;
    uint pad1;
    uint pad2;
}
[[vk::push_constant]] DownsampleParams pc;

Texture2DArray<float4>                       g_src; // level above, every face
[format("rgba16f")] RWTexture2DArray<float4> g_dst;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.dst_size || tid.y >= pc.dst_size) {
        return;
    }
    int3 base = int3(int2(tid.xy) * 2, int(tid.z));
    float4 sum = g_src.Load(int4(base, 0)) + g_src.Load(int4(base + int3(1, 0, 0), 0)) +
                 g_src.Load(int4(base + int3(0, 1, 0), 0)) +
                 g_src.Load(int4(base + int3(1, 1, 0), 0));
    g_dst[tid] = float4(sum.rgb * 0.25, 1.0);
}
