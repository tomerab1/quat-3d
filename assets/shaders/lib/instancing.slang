// instancing.slang — per-instance transforms for instanced draws (R9.1).
//
// The CPU batches draws sharing a mesh + material into one instanced draw and
// uploads a flat InstanceData array (engine::renderer::InstanceData); passes
// hand its buffer device address to the vertex shader in push constants and
// pass each draw's first instance as the draw's firstInstance.
//
// Indexing: Vulkan's InstanceIndex builtin already includes firstInstance, so
// it indexes the flat array directly. Slang's SV_InstanceID is NOT that: it
// follows D3D semantics and lowers to InstanceIndex - BaseInstance (needing the
// DrawParameters capability / shaderDrawParameters feature), i.e. it would
// drop the batch offset. Reading InstanceIndex keeps firstInstance the single
// source of the offset, which is also what indirect draws (GPU culling, R9.4)
// write per command.

// Mirrors engine::renderer::InstanceData (112 bytes, column-major model).
public struct InstanceData {
    public float4x4 model;
    // Columns of sign(det M) * cofactor(M), M = upper 3x3 of `model`
    // (|det M| * M^-T: correct under non-uniform scale and mirroring).
    // normal_c0.w = sign(det M), the tangent handedness flip.
    public float4 normal_c0;
    public float4 normal_c1;
    public float4 normal_c2;

    public float4 world_position(float3 p) {
        return mul(model, float4(p, 1.0));
    }

    // Object-space normal -> world (unnormalised; callers normalise).
    public float3 world_normal(float3 n) {
        return normal_c0.xyz * n.x + normal_c1.xyz * n.y + normal_c2.xyz * n.z;
    }

    // Object-space tangent (xyz, w = bitangent sign) -> world. The tangent is
    // a surface direction, so it transforms by the model 3x3; the bitangent
    // sign flips with a mirroring transform so cross(N, T) * w still yields the
    // transformed bitangent.
    public float4 world_tangent(float4 t) {
        float3 wt = mul((float3x3)model, t.xyz);
        return float4(wt, t.w * normal_c0.w);
    }
}

// Vulkan InstanceIndex (includes the draw's firstInstance). Vertex stage only.
public uint instance_index() {
    return spirv_asm {
        result:$$uint = OpLoad builtin(InstanceIndex:uint);
    };
}
