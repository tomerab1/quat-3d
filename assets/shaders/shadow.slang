// shadow.slang — directional shadow depth pass (Phase 7, Slice 7.1).
//
// Renders scene geometry from the light's point of view into a depth map. Depth
// only: no color attachment, the fragment shader writes nothing. The lighting
// pass samples this map to decide whether a surface is lit or shadowed.

struct PushConstants {
    float4x4 light_view_proj;
    float4x4 model;
}
[[vk::push_constant]] PushConstants pc;

struct VsIn {
    float3 position : POSITION;
}

[shader("vertex")]
float4 vertex_main(VsIn input) : SV_Position {
    float4 world = mul(pc.model, float4(input.position, 1.0));
    return mul(pc.light_view_proj, world);
}

[shader("fragment")]
void fragment_main() {}
