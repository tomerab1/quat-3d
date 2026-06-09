// bloom_up.slang — bloom upsample + accumulate (Phase 8.2).
//
// 9-tap tent filter on the smaller (already-blurred) level, added to this
// level's downsample result, building the progressively wider blur. g_base is
// this level's downsample; g_src is the next-smaller upsampled level.

[[vk::binding(0)]] Texture2D          g_src;  // smaller, upsampled level
[[vk::binding(1)]] SamplerState       g_samp;
[[vk::binding(2)]] Texture2D          g_base; // this level's downsample
[[vk::binding(3)]] RWTexture2D<float4> g_dst;

struct Params {
    float radius;
    float pad0, pad1, pad2;
};
[[vk::push_constant]] Params pc;

float3 tent9(float2 uv, float2 t) {
    float2 o = t * pc.radius;
    float3 r = g_src.SampleLevel(g_samp, uv + float2(-o.x, -o.y), 0).rgb;
    r += g_src.SampleLevel(g_samp, uv + float2(0.0, -o.y), 0).rgb * 2.0;
    r += g_src.SampleLevel(g_samp, uv + float2(o.x, -o.y), 0).rgb;
    r += g_src.SampleLevel(g_samp, uv + float2(-o.x, 0.0), 0).rgb * 2.0;
    r += g_src.SampleLevel(g_samp, uv, 0).rgb * 4.0;
    r += g_src.SampleLevel(g_samp, uv + float2(o.x, 0.0), 0).rgb * 2.0;
    r += g_src.SampleLevel(g_samp, uv + float2(-o.x, o.y), 0).rgb;
    r += g_src.SampleLevel(g_samp, uv + float2(0.0, o.y), 0).rgb * 2.0;
    r += g_src.SampleLevel(g_samp, uv + float2(o.x, o.y), 0).rgb;
    return r * (1.0 / 16.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint w = 0, h = 0;
    g_dst.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;

    uint sw = 0, sh = 0;
    g_src.GetDimensions(sw, sh);
    float2 uv = (float2(tid.xy) + 0.5) / float2(w, h);
    float3 blurred = tent9(uv, 1.0 / float2(sw, sh));
    float3 base = g_base.SampleLevel(g_samp, uv, 0).rgb;
    g_dst[tid.xy] = float4(base + blurred, 1.0);
}
