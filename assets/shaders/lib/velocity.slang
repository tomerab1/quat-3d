// velocity.slang — shader side of the velocity contract (ROADMAP R11; the
// convention is documented in engine/renderer/velocity.hpp).
//
// GBuffer writers pass two clip positions from the vertex to the fragment
// stage: this frame's rasterised (jittered) one and last frame's (unjittered,
// from the previous view-projection and the previous object pose). The stored
// value is uv_current - uv_previous with this frame's jitter removed.

// Mirrors engine::renderer::InstanceMotion (80 bytes): last frame's model
// matrix and, for skinned instances, last frame's skinned positions (one
// float4 per vertex, valid when has_prev_positions != 0; otherwise this
// frame's vertex position is reused). Presence is a flag rather than a null
// pointer test, which would need the shaderInt64 feature.
public struct InstanceMotion {
    public float4x4 prev_model;
    public float4*  prev_positions;
    public uint     has_prev_positions;
    public uint     pad;
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
