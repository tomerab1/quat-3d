// bloom_down.slang — bloom downsample (Phase 8.2).
//
// 13-tap downsample filter (Jimenez / "Next Generation Post Processing in Call
// of Duty"): high quality with good stability. The first level additionally
// applies a soft-knee bright-pass threshold so only highlights bloom, with a
// Karis luma average on the brightest taps to suppress fireflies.

[[vk::binding(0)]] Texture2D          g_src;
[[vk::binding(1)]] SamplerState       g_samp;
[[vk::binding(2)]] RWTexture2D<float4> g_dst;

struct Params {
    float threshold;
    float knee;
    float prefilter; // 1 on the first level, else 0
    float pad;
};
[[vk::push_constant]] Params pc;

float karis_weight(float3 c) {
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    return 1.0 / (1.0 + luma);
}

float3 prefilter(float3 c) {
    float br = max(c.r, max(c.g, c.b));
    float knee = max(pc.knee, 1e-4);
    float soft = clamp(br - pc.threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contrib = max(soft, br - pc.threshold) / max(br, 1e-4);
    return c * contrib;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 tid : SV_DispatchThreadID) {
    uint w = 0, h = 0;
    g_dst.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;

    uint sw = 0, sh = 0;
    g_src.GetDimensions(sw, sh);
    float2 t = 1.0 / float2(sw, sh);
    float2 uv = (float2(tid.xy) + 0.5) / float2(w, h);

    float3 a = g_src.SampleLevel(g_samp, uv + t * float2(-2, -2), 0).rgb;
    float3 b = g_src.SampleLevel(g_samp, uv + t * float2( 0, -2), 0).rgb;
    float3 c = g_src.SampleLevel(g_samp, uv + t * float2( 2, -2), 0).rgb;
    float3 d = g_src.SampleLevel(g_samp, uv + t * float2(-2,  0), 0).rgb;
    float3 e = g_src.SampleLevel(g_samp, uv + t * float2( 0,  0), 0).rgb;
    float3 f = g_src.SampleLevel(g_samp, uv + t * float2( 2,  0), 0).rgb;
    float3 g = g_src.SampleLevel(g_samp, uv + t * float2(-2,  2), 0).rgb;
    float3 hh = g_src.SampleLevel(g_samp, uv + t * float2( 0,  2), 0).rgb;
    float3 i = g_src.SampleLevel(g_samp, uv + t * float2( 2,  2), 0).rgb;
    float3 j = g_src.SampleLevel(g_samp, uv + t * float2(-1, -1), 0).rgb;
    float3 k = g_src.SampleLevel(g_samp, uv + t * float2( 1, -1), 0).rgb;
    float3 l = g_src.SampleLevel(g_samp, uv + t * float2(-1,  1), 0).rgb;
    float3 m = g_src.SampleLevel(g_samp, uv + t * float2( 1,  1), 0).rgb;

    float3 result;
    if (pc.prefilter > 0.5) {
        // Karis-weighted average of the four inner 2x2 groups -> firefly-stable.
        float3 g0 = (j + k + l + m) * 0.25;
        float3 g1 = (a + b + d + e) * 0.25;
        float3 g2 = (b + c + e + f) * 0.25;
        float3 g3 = (d + e + g + hh) * 0.25;
        float3 g4 = (e + f + hh + i) * 0.25;
        float w0 = karis_weight(g0), w1 = karis_weight(g1), w2 = karis_weight(g2);
        float w3 = karis_weight(g3), w4 = karis_weight(g4);
        result = (g0 * w0 * 0.5 + g1 * w1 * 0.125 + g2 * w2 * 0.125 + g3 * w3 * 0.125 +
                  g4 * w4 * 0.125) /
                 (w0 * 0.5 + w1 * 0.125 + w2 * 0.125 + w3 * 0.125 + w4 * 0.125);
        result = prefilter(result);
    } else {
        result = e * 0.125;
        result += (a + c + g + i) * 0.03125;
        result += (b + d + f + hh) * 0.0625;
        result += (j + k + l + m) * 0.125;
    }
    g_dst[tid.xy] = float4(result, 1.0);
}
