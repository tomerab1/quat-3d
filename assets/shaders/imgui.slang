// imgui.slang — editor UI overlay pass (Phase 9, Slice 9.1).
//
// Renders ImGui draw lists over the final swapchain image. Vertices arrive in
// framebuffer pixel coordinates (ImDrawVert: pos2 / uv2 / RGBA8 colour) and are
// mapped to clip space by a scale/translate push constant.
//
// Everything stays in gamma (sRGB-encoded) space, as ImGui authors it: vertex
// colours pass through unmodified, textures are bound through UNORM views
// (stored bytes, no decode) and the attachment is the target's UNORM view, so
// blending happens on the encoded values (issue #92: blending glyph coverage
// in linear space thinned dark text on light backgrounds to grey).

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
    float4 col : COLOR0; // RGBA8 UNORM, sRGB-encoded (used as is)
}

struct VsOut {
    float4 clip : SV_Position;
    float2 uv   : TEXCOORD0;
    float4 col  : COLOR0;
}

[shader("vertex")]
VsOut vertex_main(VsIn input) {
    VsOut o;
    o.clip = float4(input.pos * pc.scale + pc.translate, 0.0, 1.0);
    o.uv = input.uv;
    o.col = input.col;
    return o;
}

[shader("fragment")]
float4 fragment_main(VsOut input) : SV_Target0 {
    return input.col * g_texture.Sample(g_sampler, input.uv);
}
