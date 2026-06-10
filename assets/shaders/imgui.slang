// imgui.slang — editor UI overlay pass (Phase 9, Slice 9.1).
//
// Renders ImGui draw lists over the final swapchain image. Vertices arrive in
// framebuffer pixel coordinates (ImDrawVert: pos2 / uv2 / RGBA8 colour) and are
// mapped to clip space by a scale/translate push constant. Vertex colours are
// authored in sRGB; they are linearised here so the sRGB swapchain attachment
// re-encodes them correctly (and blending happens in linear space).

struct PushConstants {
    float2 scale;     // 2 / display_size
    float2 translate; // -1 - display_pos * scale
}
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0)]] Texture2D    g_texture; // font atlas (or any ImTextureID image)
[[vk::binding(1)]] SamplerState g_sampler;

struct VsIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0; // RGBA8 UNORM, sRGB-authored
}

struct VsOut {
    float4 clip : SV_Position;
    float2 uv   : TEXCOORD0;
    float4 col  : COLOR0;
}

// Piecewise sRGB EOTF (the exact inverse the sRGB attachment encode applies).
float3 srgb_to_linear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return select(c <= 0.04045, lo, hi);
}

[shader("vertex")]
VsOut vertex_main(VsIn input) {
    VsOut o;
    o.clip = float4(input.pos * pc.scale + pc.translate, 0.0, 1.0);
    o.uv = input.uv;
    o.col = float4(srgb_to_linear(input.col.rgb), input.col.a);
    return o;
}

[shader("fragment")]
float4 fragment_main(VsOut input) : SV_Target0 {
    return input.col * g_texture.Sample(g_sampler, input.uv);
}
