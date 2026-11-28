// wind_field.slang — evaluates the global wind field into a texture around
// the camera (R13.1, WindFieldPass). One thread per texel; each texel holds
// the field at its centre (lib/wind_common.slang, the GPU mirror of the CPU
// reference engine::weather::sample_wind). Grass samples the texture.

import wind_common;

[[vk::push_constant]] WindFieldGpu pc;

[format("rgba16f")] RWTexture2D<float4> g_wind; // r vel.x, g vel.z, b vel.y, a gust

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint width = 0;
    uint height = 0;
    g_wind.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;
    // Texel centre: corner + (index + 0.5) * texel size (int -> float and one
    // multiply-add, the same on the CPU side of the tests).
    float2 xz = pc.grid.xy + (float2(tid.xy) + 0.5) * pc.grid.z;
    float4 w = wind_field_sample(pc, xz);
    g_wind[int2(tid.xy)] = float4(w.x, w.z, w.y, w.w);
}
