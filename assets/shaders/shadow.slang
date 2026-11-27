// shadow.slang — directional shadow depth pass (Phase 7, Slice 7.1).
//
// Renders scene geometry from the light's point of view into a depth map. Depth
// only: no color attachment, the fragment shader writes nothing. The lighting
// pass samples this map to decide whether a surface is lit or shadowed. Draws
// are instanced (R9.1), reading per-instance transforms like the GBuffer pass.

import instancing;

// Mirrors engine::renderer::ShadowPushConstants (72 bytes).
struct PushConstants {
    float4x4      light_view_proj;
    InstanceData* instances; // indexed by instance_index() (includes firstInstance)
}
[[vk::push_constant]] PushConstants pc;

struct VsIn {
    float3 position : POSITION;
}

[shader("vertex")]
float4 vertex_main(VsIn input) : SV_Position {
    InstanceData inst = pc.instances[instance_index()];
    return mul(pc.light_view_proj, inst.world_position(input.position));
}

[shader("fragment")]
void fragment_main() {}
