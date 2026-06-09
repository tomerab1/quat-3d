// tonemap.slang — tonemap + present pass (Phase 3, Slice 3.7).
//
// A fullscreen triangle samples the linear HDR colour produced by the lighting
// pass, applies the ACES filmic curve (Narkowicz fit) and sRGB transfer, and
// writes the LDR result to the output colour target (the swapchain image in the
// live path, a transient image in the self-test). The output format is a plain
// UNORM target, so the sRGB encode is done explicitly here.

struct VsOut {
    float4 clip : SV_Position;
    float2 uv   : TEXCOORD0;
}

// Fullscreen triangle from the vertex id — no vertex buffer. uv spans [0,2] so
// the visible [-1,1] clip region maps to [0,1] texture space (Vulkan top-left).
[shader("vertex")]
VsOut vertex_main(uint id : SV_VertexID) {
    VsOut o;
    float2 uv = float2(float((id << 1) & 2), float(id & 2));
    o.uv = uv;
    o.clip = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[[vk::binding(0)]] Texture2D               g_hdr;
[[vk::binding(1)]] SamplerState            g_sampler;
[[vk::binding(2)]] StructuredBuffer<float> g_exposure; // [0] = adapted avg luminance

// Exposure controls. auto_exposure 0 -> multiplier 1 (the pixel-exact self-test
// path); 1 -> key / adapted-luminance, clamped to [min, max].
struct TonemapParams {
    float auto_exposure;
    float key;
    float min_exposure;
    float max_exposure;
};
[[vk::push_constant]] TonemapParams pc;

float3 aces(float3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 linear_to_srgb(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return select(c <= 0.0031308, lo, hi);
}

[shader("fragment")]
float4 fragment_main(VsOut input) : SV_Target {
    float3 hdr = g_hdr.Sample(g_sampler, input.uv).rgb;
    float exposure = 1.0;
    if (pc.auto_exposure > 0.5) {
        float avg = max(g_exposure[0], 1e-4);
        exposure = clamp(pc.key / avg, pc.min_exposure, pc.max_exposure);
    }
    float3 ldr = linear_to_srgb(aces(hdr * exposure));
    return float4(ldr, 1.0);
}
