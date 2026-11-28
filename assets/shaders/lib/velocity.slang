// velocity.slang — shader side of the velocity contract (ROADMAP R11; the
// convention is documented in engine/renderer/velocity.hpp).
//
// GBuffer writers pass two clip positions from the vertex to the fragment
// stage: this frame's rasterised (jittered) one and last frame's (unjittered,
// from the previous view-projection and the previous object pose). The stored
// value is uv_current - uv_previous with this frame's jitter removed.

// Mirrors engine::renderer::InstanceMotion (64 bytes): last frame's model
// matrix. Last frame's skinned positions are per draw, not per instance (a
// skinned mesh is always its own single-instance draw), so they come through
// the pass's push constants; see GBufferPushConstants.prev_positions.
public struct InstanceMotion {
    public float4x4 prev_model;
}

// One vertex of last frame's skinned positions (lib/skinning.slang writes
// them). A struct rather than a bare float4 pointer: older SPIRV-Cross
// releases (MoltenVK's SPIR-V -> MSL translator) size and align a
// pointer-to-vector member like the vector itself and reject the block.
public struct PrevPosition {
    public float4 position; // xyz, w = 1
}

// Unjittered UV motion of a surface point (velocity.hpp convention).
public float2 surface_velocity(float4 clip_jittered, float4 prev_clip, float2 jitter_ndc) {
    const float2 cur = clip_jittered.xy / max(clip_jittered.w, 1e-6) - jitter_ndc;
    const float2 prev = prev_clip.xy / max(prev_clip.w, 1e-6);
    return (cur - prev) * 0.5;
}

// Vulkan VertexIndex (includes the draw's vertexOffset; the engine's indexed
// draws use 0). Vertex stage only. Read directly, like instance_index(), so
// no DrawParameters capability is needed.
public uint vertex_index() {
    return spirv_asm {
        result:$$uint = OpLoad builtin(VertexIndex:uint);
    };
}
