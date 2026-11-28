// velocity_background.slang — camera-only velocity for the background (R11).
//
// The GBuffer writers leave the velocity target at its clear value where no
// geometry was drawn (sky). A fullscreen triangle at depth 1, depth-tested
// EQUAL against the GBuffer depth, fills exactly those pixels with the motion
// of the far-plane point they show, so the target is complete for TAA and the
// upscalers. The far point stays homogeneous (no divide), which also covers
// projections whose far plane is at infinity. Same convention as
// lib/velocity.slang (unjittered uv_current - uv_previous).

struct PushConstants {
    float4x4 inv_view_proj;  // this frame, unjittered
    float4x4 prev_view_proj; // last frame, unjittered
    float4   params;         // xy this frame's jitter (NDC), zw 1 / render extent
}
[[vk::push_constant]] PushConstants pc;

struct VsOut {
    float4 clip : SV_Position;
}

[shader("vertex")]
VsOut vertex_main(uint id : SV_VertexID) {
    const float2 p = float2(float((id << 1) & 2u), float(id & 2u)); // (0,0) (2,0) (0,2)
    VsOut o;
    o.clip = float4(p * 2.0 - 1.0, 1.0, 1.0);
    return o;
}

[shader("fragment")]
float2 fragment_main(VsOut input) : SV_Target {
    // SV_Position holds the pixel centre; remove the jitter to get the
    // unjittered NDC of the scene point the pixel shows.
    const float2 ndc = input.clip.xy * pc.params.zw * 2.0 - 1.0 - pc.params.xy;
    const float4 far_point = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));
    const float4 prev_clip = mul(pc.prev_view_proj, far_point);
    // The homogeneous scale of far_point cancels in xy / w (sign included).
    const float w = abs(prev_clip.w) < 1e-20 ? 1e-20 : prev_clip.w;
    return (ndc - prev_clip.xy / w) * 0.5;
}
