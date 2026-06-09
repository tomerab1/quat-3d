// transparent.slang — forward alpha-blended pass (Phase 7, Slice 7.4).
//
// Renders alpha-blended geometry over the deferred (opaque) HDR result, depth-
// tested against the opaque GBuffer depth (no depth write). Forward-shades with
// the shared PBR BRDF + one directional light + ambient, outputting premultiply-
// free RGBA for SRC_ALPHA / ONE_MINUS_SRC_ALPHA blending.

import material;
import pbr;

struct PushConstants {
    float4x4 view_proj;
    float4x4 model;
    float4   camera_pos;      // xyz
    float4   light_direction; // xyz = direction TO the light
    float4   light_color;     // rgb colour, w intensity
    float4   ambient;         // rgb ambient
}
[[vk::push_constant]] PushConstants pc;

ParameterBlock<PbrMaterial> g_material;

struct VsIn {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 tangent  : TANGENT;
}

struct VsOut {
    float4 clip      : SV_Position;
    float3 world_pos : POSITION0;
    float3 world_n   : NORMAL;
    float4 world_t   : TANGENT;
    float2 uv        : TEXCOORD0;
}

[shader("vertex")]
VsOut vertex_main(VsIn input) {
    VsOut o;
    float4 world = mul(pc.model, float4(input.position, 1.0));
    o.world_pos = world.xyz;
    o.clip = mul(pc.view_proj, world);
    float3x3 nm = (float3x3)pc.model;
    o.world_n = normalize(mul(nm, input.normal));
    o.world_t = float4(normalize(mul(nm, input.tangent.xyz)), input.tangent.w);
    o.uv = input.uv;
    return o;
}

[shader("fragment")]
float4 fragment_main(VsOut input) : SV_Target0 {
    SurfaceSample s = g_material.evaluate(input.uv);

    float3 n = normalize(input.world_n);
    float3 t = normalize(input.world_t.xyz - n * dot(n, input.world_t.xyz));
    float3 b = cross(n, t) * input.world_t.w;
    float3 world_n = normalize(t * s.normal_ts.x + b * s.normal_ts.y + n * s.normal_ts.z);

    float3 V = normalize(pc.camera_pos.xyz - input.world_pos);
    float3 L = normalize(pc.light_direction.xyz);
    float3 lit = pbr_surface_brdf(world_n, V, L, s.albedo, s.metallic, s.roughness) *
                 pc.light_color.rgb * pc.light_color.w;
    float3 ambient = pc.ambient.rgb * s.albedo * s.occlusion;
    return float4(lit + ambient + s.emissive, s.alpha);
}
