// shadow.slang — cascaded sun shadow depth pass (Phase 7; cascades in R12).
//
// Renders scene geometry into one layer of the cascaded shadow map: depth
// only, no colour attachment, the fragment shader writes nothing. The
// cascade's view-projection comes from the CascadeData buffer (fitted on the
// GPU by csm_setup.slang, or uploaded by the CPU fallback), read through a
// buffer device address. Draws are instanced (R9.1), reading per-instance
// transforms like the GBuffer pass.

import csm;
import instancing;

// Mirrors engine::renderer::ShadowPushConstants (24 bytes).
struct PushConstants {
    CascadeData*  cascades;
    InstanceData* instances; // indexed by instance_index() (includes firstInstance)
    uint          cascade;
}
[[vk::push_constant]] PushConstants pc;

struct VsIn {
    float3 position : POSITION;
}

[shader("vertex")]
float4 vertex_main(VsIn input) : SV_Position {
    InstanceData inst = pc.instances[instance_index()];
    float4 p = inst.world_position(input.position);
    uint b = pc.cascade * 4;
    return pc.cascades[0].view_proj[b] * p.x + pc.cascades[0].view_proj[b + 1] * p.y +
           pc.cascades[0].view_proj[b + 2] * p.z + pc.cascades[0].view_proj[b + 3] * p.w;
}

[shader("fragment")]
void fragment_main() {}
