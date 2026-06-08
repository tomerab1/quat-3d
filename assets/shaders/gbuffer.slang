// gbuffer.slang — deferred GBuffer pass (Phase 3, Slice 3.5).
//
// Rasterises meshes with the standard interleaved vertex layout and writes the
// deferred GBuffer: albedo, octahedral-encoded world normal, and packed
// metallic/roughness/occlusion. Shading inputs come from the material via the
// IMaterial interface (PbrMaterial), bound as a descriptor-buffer parameter
// block. Per-draw transforms arrive as push constants.

import material;

// Per-draw transforms. 128 bytes (two 4x4 matrices).
struct PushConstants {
    float4x4 view_proj;
    float4x4 model;
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
}

[shader("vertex")]
VsOut vertex_main(VsIn input) {
    VsOut o;
    float4 world_pos = mul(pc.model, float4(input.position, 1.0));
    o.clip = mul(pc.view_proj, world_pos);

    // Normal matrix: assumes uniform scale, so the model's upper-left 3x3 works.
    float3x3 nm = (float3x3)pc.model;
    o.world_n = normalize(mul(nm, input.normal));
    o.world_t = float4(normalize(mul(nm, input.tangent.xyz)), input.tangent.w);
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
    float4 albedo   : SV_Target0;   // rgb albedo, a = alpha
    float2 normal   : SV_Target1;   // oct-encoded world normal
    float4 material : SV_Target2;   // r metallic, g roughness, b occlusion
}

[shader("fragment")]
GBufferOut fragment_main(VsOut input) {
    SurfaceSample s = g_material.evaluate(input.uv);

    // Build the world-space normal from the tangent-space material normal.
    float3 n = normalize(input.world_n);
    float3 t = normalize(input.world_t.xyz - n * dot(n, input.world_t.xyz));
    float3 b = cross(n, t) * input.world_t.w;
    float3 world_n = normalize(t * s.normal_ts.x + b * s.normal_ts.y + n * s.normal_ts.z);

    GBufferOut o;
    o.albedo   = float4(s.albedo, s.alpha);
    o.normal   = oct_encode(world_n);
    o.material = float4(s.metallic, s.roughness, s.occlusion, 1.0);
    return o;
}
