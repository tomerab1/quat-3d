// bloom_composite.slang — add the bloom back into the HDR buffer (Phase 8.2).
//
// Tent-upsamples the half-resolution bloom result to full resolution and adds it
// into the HDR colour (read-modify-write) scaled by intensity, before tonemap.
// The sum is clamped to the half-float range (lib/hdr.slang): a pixel already
// at 65504 plus bloom would otherwise round to +Inf.

import hdr;

Texture2D          g_bloom; // half-res accumulated bloom
SamplerState       g_samp;
[format("rgba16f")] RWTexture2D<float4> g_hdr;   // full-res HDR (in/out)

struct Params {
    float intensity;
    float radius;
    float pad0, pad1;
};
[[vk::push_constant]] Params pc;

float3 tent9(float2 uv, float2 t) {
    float2 o = t * pc.radius;
    float3 r = g_bloom.SampleLevel(g_samp, uv + float2(-o.x, -o.y), 0).rgb;
    r += g_bloom.SampleLevel(g_samp, uv + float2(0.0, -o.y), 0).rgb * 2.0;
    r += g_bloom.SampleLevel(g_samp, uv + float2(o.x, -o.y), 0).rgb;
    r += g_bloom.SampleLevel(g_samp, uv + float2(-o.x, 0.0), 0).rgb * 2.0;
    r += g_bloom.SampleLevel(g_samp, uv, 0).rgb * 4.0;
    r += g_bloom.SampleLevel(g_samp, uv + float2(o.x, 0.0), 0).rgb * 2.0;
    r += g_bloom.SampleLevel(g_samp, uv + float2(-o.x, o.y), 0).rgb;
    r += g_bloom.SampleLevel(g_samp, uv + float2(0.0, o.y), 0).rgb * 2.0;
    r += g_bloom.SampleLevel(g_samp, uv + float2(o.x, o.y), 0).rgb;
    return r * (1.0 / 16.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint w = 0, h = 0;
    g_hdr.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;

    uint bw = 0, bh = 0;
    g_bloom.GetDimensions(bw, bh);
    float2 uv = (float2(tid.xy) + 0.5) / float2(w, h);
    float3 bloom = tent9(uv, 1.0 / float2(bw, bh));
    float3 base = g_hdr[tid.xy].rgb;
    g_hdr[tid.xy] = float4(hdr_store(base + bloom * pc.intensity), 1.0);
}
