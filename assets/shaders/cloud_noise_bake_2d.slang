// cloud_noise_bake_2d.slang — bakes the clouds' 2D noise textures (ROADMAP
// R13.4, CloudNoisePass): texture 0 is the curl noise (rg, 0.5 = none),
// texture 1 the weather noise (coverage / type / precipitation fields); see
// lib/cloud_noise.slang. Run once at startup.

import cloud_noise;

struct BakePush {
    uint texture; // 0 curl, 1 weather
    uint size;    // edge in texels
    uint pad0;
    uint pad1;
}
[[vk::push_constant]] BakePush pc;

[format("rgba8")] RWTexture2D<float4> g_texture;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= pc.size || tid.y >= pc.size) return;
    float2 uv = (float2(tid.xy) + 0.5) / float(pc.size);
    g_texture[tid.xy] =
        pc.texture == 0u ? float4(cloud_curl_noise(uv), 0.0, 1.0) : cloud_weather_noise(uv);
}
