// gbuffer.slang — deferred GBuffer pass (Phase 3, Slice 3.5).
//
// Rasterises meshes with the standard interleaved vertex layout and writes the
// deferred GBuffer: albedo, octahedral-encoded world normal, and packed
// metallic/roughness/occlusion. Shading inputs come from the material via the
// IMaterial interface (PbrMaterial), bound as a descriptor-buffer parameter
// block. Draws are instanced (R9.1): per-instance model + normal matrices are
// read through a buffer device address (lib/instancing.slang).
//
// Two fragment entry points share one body, specialised on the alpha mode
// (lib/alpha_test.slang): fragment_main for opaque materials (no discard, so
// the pipeline keeps early depth testing) and fragment_masked for glTF MASK
// materials (discards below alphaCutoff; R12.11). Back faces of doubleSided
// materials are shaded with the reversed normal, as glTF requires; a mirrored
// instance (negative determinant, glTF 3.7.4) reverses its winding, so its
// front faces rasterise as back faces and the test compensates for that.
//
// Everything a GBuffer writer outputs is computed in gbuffer_shade<A>, so the
// opaque and masked entry points always write the same targets
// (alpha_variant_shader_test checks this on the SPIR-V).

import alpha_test;
import instancing;
import material;
import pbr;

// Mirrors engine::renderer::GBufferPushConstants (72 bytes).
struct PushConstants {
    float4x4      view_proj;
    InstanceData* instances; // indexed by instance_index() (includes firstInstance)
}
[[vk::push_constant]] PushConstants pc;

// Material parameter block (constant buffer + textures + sampler) in its own
// descriptor set, bound per draw via a descriptor buffer.
ParameterBlock<PbrMaterial> g_material;

struct VsIn {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 tangent  : TANGENT;   // xyz tangent, w bitangent sign
}

struct VsOut {
    float4 clip    : SV_Position;
    float3 world_n : NORMAL;
    float4 world_t : TANGENT;
    float2 uv      : TEXCOORD0;
    // 1 when the instance transform mirrors (det < 0): its winding is reversed.
    nointerpolation float mirrored : TEXCOORD1;
}

[shader("vertex")]
VsOut vertex_main(VsIn input) {
    InstanceData inst = pc.instances[instance_index()];
    VsOut o;
    o.clip = mul(pc.view_proj, inst.world_position(input.position));

    // Cofactor normal matrix (non-uniform scale) + mirrored tangent handedness.
    o.world_n = normalize(inst.world_normal(input.normal));
    float4 t = inst.world_tangent(input.tangent);
    o.world_t = float4(normalize(t.xyz), t.w);
    o.uv = input.uv;
    o.mirrored = inst.normal_c0.w < 0.0 ? 1.0 : 0.0; // sign(det M), see InstanceData
    return o;
}

// Component-wise sign that maps zero to +1 (octahedral encoding convention).
float2 sign_nonzero(float2 v) {
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Octahedral encode a unit vector into [-1, 1]^2 (stored in an RG16F target).
float2 oct_encode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = n.xy;
    if (n.z < 0.0) {
        e = (1.0 - abs(float2(e.y, e.x))) * sign_nonzero(e);
    }
    return e;
}

struct GBufferOut {
    float4 albedo    : SV_Target0;   // rgb albedo, a = alpha
    float2 normal    : SV_Target1;   // oct-encoded world normal
    float4 material  : SV_Target2;   // r metallic, g roughness, b occlusion
    float2 clearcoat : SV_Target3;   // r coat strength, g coat perceptual roughness
}

GBufferOut gbuffer_shade<A : IAlphaMode>(VsOut input, bool front_face) {
    SurfaceSample s = g_material.evaluate(input.uv);
    A::test(s.alpha, g_material.params.alpha_cutoff);

    // Build the world-space normal from the tangent-space material normal.
    // glTF doubleSided: the back face is lit with the reversed normal (the
    // tangent frame flips with it, keeping the normal map's handedness).
    // SV_IsFrontFace follows screen winding; a mirrored instance's outside
    // faces wind clockwise, so they arrive as back faces.
    bool outside = front_face != (input.mirrored > 0.5);
    float3 n = normalize(input.world_n);
    if (!outside && (g_material.params.flags & MATERIAL_DOUBLE_SIDED) != 0) {
        n = -n;
    }
    float3 t = normalize(input.world_t.xyz - n * dot(n, input.world_t.xyz));
    float3 b = cross(n, t) * input.world_t.w;
    float3 world_n = normalize(t * s.normal_ts.x + b * s.normal_ts.y + n * s.normal_ts.z);

    // Specular AA at GBuffer-write time (the deferred lighting pass is compute
    // and has no derivatives): fold sub-pixel normal variance into roughness.
    float roughness = pbr_specular_aa(world_n, s.roughness);
    float coat_roughness = pbr_specular_aa(world_n, s.clearcoat_roughness);

    GBufferOut o;
    o.albedo    = float4(s.albedo, s.alpha);
    o.normal    = oct_encode(world_n);
    o.material  = float4(s.metallic, roughness, s.occlusion, 1.0);
    o.clearcoat = float2(s.clearcoat, coat_roughness);
    return o;
}

[shader("fragment")]
GBufferOut fragment_main(VsOut input, bool front_face : SV_IsFrontFace) {
    return gbuffer_shade<AlphaOpaque>(input, front_face);
}

[shader("fragment")]
GBufferOut fragment_masked(VsOut input, bool front_face : SV_IsFrontFace) {
    return gbuffer_shade<AlphaMasked>(input, front_face);
}
