// transparent.slang — forward alpha-blend + glass transmission (Phase 7, 7.4/7.5).
//
// Blends over the deferred HDR. Plain BLEND materials use SRC_ALPHA blending.
// Transmission materials (KHR_materials_transmission) sample the copied opaque
// scene colour at a normal-refracted screen offset, tint it (Beer-Lambert-ish),
// and add the surface's specular highlight — see-through glass. Material textures
// are bound explicitly (b0..6) so the scene-colour texture can share set 0.

import pbr;

static const uint MAT_HAS_BASE_COLOR = 1u << 0;
static const uint MAT_HAS_NORMAL     = 1u << 1;
static const uint MAT_HAS_MR         = 1u << 2;
static const uint MAT_HAS_EMISSIVE   = 1u << 3;
static const uint MAT_HAS_OCCLUSION  = 1u << 4;
static const uint MAT_TRANSMISSION   = 1u << 6;

// Mirrors engine::asset::PbrMaterialParams (std140, 64 bytes). emissive_factor.w
// carries the transmission factor.
struct MaterialParams {
    float4 base_color_factor;
    float4 emissive_factor;
    float  metallic_factor;
    float  roughness_factor;
    float  occlusion_strength;
    float  normal_scale;
    float  alpha_cutoff;
    uint   flags;
    float2 pad;
}

struct PushConstants {
    float4x4 view_proj;
    float4x4 model;
    float4   camera_pos;      // xyz
    float4   light_direction; // xyz = to light, w = screen width
    float4   light_color;     // rgb, w intensity
    float4   ambient;         // rgb, w = screen height
}
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0)]] ConstantBuffer<MaterialParams> g_params;
[[vk::binding(1)]] Texture2D    g_base_color;
[[vk::binding(2)]] Texture2D    g_normal;
[[vk::binding(3)]] Texture2D    g_metallic_roughness;
[[vk::binding(4)]] Texture2D    g_emissive;
[[vk::binding(5)]] Texture2D    g_occlusion;
[[vk::binding(6)]] SamplerState g_samp;
[[vk::binding(7)]] Texture2D    g_scene_color;   // copied opaque HDR
[[vk::binding(8)]] SamplerState g_scene_sampler;

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
    float4 base = g_params.base_color_factor;
    if ((g_params.flags & MAT_HAS_BASE_COLOR) != 0) {
        base *= g_base_color.Sample(g_samp, input.uv);
    }
    float3 albedo = base.rgb;
    float  alpha = base.a;

    float metallic = g_params.metallic_factor;
    float roughness = g_params.roughness_factor;
    if ((g_params.flags & MAT_HAS_MR) != 0) {
        float3 mr = g_metallic_roughness.Sample(g_samp, input.uv).rgb;
        roughness *= mr.g;
        metallic *= mr.b;
    }
    float3 nts = float3(0, 0, 1);
    if ((g_params.flags & MAT_HAS_NORMAL) != 0) {
        nts = g_normal.Sample(g_samp, input.uv).xyz * 2.0 - 1.0;
        nts = normalize(float3(nts.xy * g_params.normal_scale, nts.z));
    }
    float occ = 1.0;
    if ((g_params.flags & MAT_HAS_OCCLUSION) != 0) {
        occ = lerp(1.0, g_occlusion.Sample(g_samp, input.uv).r, g_params.occlusion_strength);
    }
    float3 emissive = g_params.emissive_factor.rgb;
    if ((g_params.flags & MAT_HAS_EMISSIVE) != 0) {
        emissive *= g_emissive.Sample(g_samp, input.uv).rgb;
    }

    float3 n = normalize(input.world_n);
    float3 t = normalize(input.world_t.xyz - n * dot(n, input.world_t.xyz));
    float3 b = cross(n, t) * input.world_t.w;
    float3 world_n = normalize(t * nts.x + b * nts.y + n * nts.z);

    float3 V = normalize(pc.camera_pos.xyz - input.world_pos);
    // Two-sided shading: thin glass / blended geometry is rendered with culling off,
    // so flip the shading normal to face the viewer (otherwise a back face yields
    // ndotl = ndotv = 0 and the surface goes black).
    if (dot(world_n, V) < 0.0) world_n = -world_n;
    float3 L = normalize(pc.light_direction.xyz);
    float3 lit = pbr_surface_brdf(world_n, V, L, albedo, metallic, roughness) * pc.light_color.rgb *
                 pc.light_color.w;
    float3 ambient = pc.ambient.rgb * albedo * occ;

    if ((g_params.flags & MAT_TRANSMISSION) != 0) {
        // See-through glass: sample the opaque scene behind, distorted by the
        // surface normal, tinted by the base colour, plus the specular highlight.
        float2 screen_uv = input.clip.xy / float2(pc.light_direction.w, pc.ambient.w);
        float2 refract = world_n.xy * (0.06 * (1.0 - roughness));
        float3 background = g_scene_color.Sample(g_scene_sampler, screen_uv + refract).rgb;
        float ndotv = max(dot(world_n, V), 0.0);
        float fresnel = 0.04 + 0.96 * pow(1.0 - ndotv, 5.0);
        float3 glass = background * albedo * (1.0 - fresnel) + lit + emissive;
        return float4(glass, 1.0); // background already composited
    }

    return float4(lit + ambient + emissive, alpha);
}
