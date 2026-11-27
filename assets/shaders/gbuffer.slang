// gbuffer.slang — deferred GBuffer pass (Phase 3, Slice 3.5).
//
// Rasterises meshes with the standard interleaved vertex layout and writes the
// deferred GBuffer: albedo, octahedral-encoded world normal, and packed
// metallic/roughness/occlusion. Shading inputs come from the material via the
// IMaterial interface (PbrMaterial), bound as a descriptor-buffer parameter
// block. Draws are instanced (R9.1): per-instance model + normal matrices are
// read through a buffer device address (lib/instancing.slang). The velocity
// target (R11, lib/velocity.slang) gets each vertex's motion from the
// per-instance previous model matrix (and previous skinned position).

import instancing;
import material;
import pbr;
import velocity;

// Mirrors engine::renderer::GBufferPushConstants (160 bytes).
struct PushConstants {
    float4x4        view_proj;      // this frame, jittered
    float4x4        prev_view_proj; // last frame, unjittered
    InstanceData*   instances;      // indexed by instance_index() (includes firstInstance)
    InstanceMotion* motion;         // parallel to instances, valid when has_motion != 0
    float2          jitter_ndc;     // this frame's projection jitter
    uint            has_motion;     // 0: camera-only velocity (no previous poses)
    uint            pad;
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
    float4 clip      : SV_Position;
    float3 world_n   : NORMAL;
    float4 world_t   : TANGENT;
    float2 uv        : TEXCOORD0;
    float4 cur_clip  : CUR_CLIP;  // = clip, interpolated as a plain varying
    float4 prev_clip : PREV_CLIP; // last frame's clip position (unjittered)
}

[shader("vertex")]
VsOut vertex_main(VsIn input) {
    const uint instance = instance_index();
    InstanceData inst = pc.instances[instance];
    VsOut o;
    o.clip = mul(pc.view_proj, inst.world_position(input.position));

    float4x4 prev_model = inst.model;
    float3 prev_position = input.position;
    if (pc.has_motion != 0) {
        const InstanceMotion m = pc.motion[instance];
        prev_model = m.prev_model;
        if (m.has_prev_positions != 0) prev_position = m.prev_positions[vertex_index()].xyz;
    }
    o.cur_clip = o.clip;
    o.prev_clip = mul(pc.prev_view_proj, mul(prev_model, float4(prev_position, 1.0)));

    // Cofactor normal matrix (non-uniform scale) + mirrored tangent handedness.
    o.world_n = normalize(inst.world_normal(input.normal));
    float4 t = inst.world_tangent(input.tangent);
    o.world_t = float4(normalize(t.xyz), t.w);
    o.uv = input.uv;
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
    float2 velocity  : SV_Target4;   // uv motion (lib/velocity.slang)
}

[shader("fragment")]
GBufferOut fragment_main(VsOut input) {
    SurfaceSample s = g_material.evaluate(input.uv);

    // Build the world-space normal from the tangent-space material normal.
    float3 n = normalize(input.world_n);
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
    o.velocity  = surface_velocity(input.cur_clip, input.prev_clip, pc.jitter_ndc);
    return o;
}
