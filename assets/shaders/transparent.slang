// transparent.slang — forward alpha-blend + glass transmission (Phase 7).
//
// Blends over the deferred HDR. Plain BLEND materials use SRC_ALPHA blending.
// Transmission materials (KHR_materials_transmission + _volume + _ior) refract
// the view ray through the medium: the exit point is projected back to screen
// space and the opaque scene colour is sampled from a pre-blurred mip chain
// (mip = f(roughness, ior)), tinted by Beer-Lambert volume attenuation, and
// combined with split-sum IBL reflection + the sun's specular highlight.
// Material textures are bound explicitly (b0..6) so the scene-colour snapshot
// (b7..8) and IBL maps (b9..12) can share set 0.

import pbr;

static const uint MAT_HAS_BASE_COLOR = 1u << 0;
static const uint MAT_HAS_NORMAL     = 1u << 1;
static const uint MAT_HAS_MR         = 1u << 2;
static const uint MAT_HAS_EMISSIVE   = 1u << 3;
static const uint MAT_HAS_OCCLUSION  = 1u << 4;
static const uint MAT_TRANSMISSION   = 1u << 6;
static const uint MAT_HAS_THICKNESS  = 1u << 8;

// Prefiltered specular cubemap mip count - 1. Must match ibl_pass.cpp.
static const float PREFILTER_MAX_LOD = 4.0;

// Mirrors engine::asset::PbrMaterialParams (std140, 96 bytes).
struct MaterialParams {
    float4 base_color_factor;
    float4 emissive_factor;     // xyz used; w is padding
    float  metallic_factor;
    float  roughness_factor;
    float  occlusion_strength;
    float  normal_scale;
    float  alpha_cutoff;
    uint   flags;
    float  transmission_factor; // KHR_materials_transmission
    float  ior;                 // KHR_materials_ior
    float4 attenuation;         // rgb = attenuation colour, w = distance (0 = none)
    float  thickness;           // KHR_materials_volume (local units)
    float3 pad;
}

struct PushConstants {
    float4x4 view_proj;
    float4x4 model;
    float4   camera_pos;      // xyz
    float4   light_direction; // xyz = to light, w = screen width (unused)
    float4   light_color;     // rgb, w intensity
    float4   ambient;         // rgb, w = screen height (unused)
}
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0)]]  ConstantBuffer<MaterialParams> g_params;
[[vk::binding(1)]]  Texture2D    g_base_color;
[[vk::binding(2)]]  Texture2D    g_normal;
[[vk::binding(3)]]  Texture2D    g_metallic_roughness;
[[vk::binding(4)]]  Texture2D    g_emissive;
[[vk::binding(5)]]  Texture2D    g_occlusion;
[[vk::binding(6)]]  SamplerState g_samp;
[[vk::binding(7)]]  Texture2D    g_scene_color;   // copied opaque HDR, blurred mip chain
[[vk::binding(8)]]  SamplerState g_scene_sampler;
[[vk::binding(9)]]  TextureCube  g_irradiance;    // split-sum IBL (glass reflections)
[[vk::binding(10)]] TextureCube  g_prefiltered;
[[vk::binding(11)]] Texture2D    g_brdf_lut;
[[vk::binding(12)]] SamplerState g_ibl_sampler;
[[vk::binding(13)]] Texture2D    g_thickness;     // KHR_materials_volume (G channel)

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

// Schlick Fresnel with a roughness cap, for the view-dependent environment terms.
float3 fresnel_schlick_roughness(float ndotv, float3 f0, float roughness) {
    float3 fmax = max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), f0);
    return f0 + (fmax - f0) * pow(saturate(1.0 - ndotv), 5.0);
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
    // Two-sided shading: blended geometry (and doubleSided glass) is rendered
    // with culling off, so flip the shading normal to face the viewer.
    if (dot(world_n, V) < 0.0) world_n = -world_n;
    float3 L = normalize(pc.light_direction.xyz);

    if ((g_params.flags & MAT_TRANSMISSION) != 0) {
        float ndotv = max(dot(world_n, V), 1e-4);
        // Dielectric reflectance from the real index of refraction.
        float f0s = (g_params.ior - 1.0) / (g_params.ior + 1.0);
        float3 f0 = lerp(float3(f0s * f0s), albedo, metallic);
        float3 F = fresnel_schlick_roughness(ndotv, f0, roughness);

        // Refract the view ray into the medium, walk the volume thickness
        // (scaled by the node transform) and project the exit point back to
        // screen space. thickness = 0 (thin surface) degenerates to the
        // current pixel — tint and blur with no spatial offset.
        float scale = length(mul((float3x3)pc.model, float3(0.577350, 0.577350, 0.577350)));
        float thickness_world = g_params.thickness * scale;
        if ((g_params.flags & MAT_HAS_THICKNESS) != 0) {
            thickness_world *= g_thickness.Sample(g_samp, input.uv).g;
        }
        float3 refr = refract(-V, world_n, 1.0 / g_params.ior);
        float3 exit_pos = input.world_pos + refr * thickness_world;
        float4 exit_clip = mul(pc.view_proj, float4(exit_pos, 1.0));
        float2 exit_uv = saturate(exit_clip.xy / max(exit_clip.w, 1e-4) * 0.5 + 0.5);

        // Rough transmission: sample the pre-blurred scene mip chain. The LOD
        // ramp follows the glTF sample viewer (ior tightens the blur).
        uint sw = 0, sh = 0, levels = 0;
        g_scene_color.GetDimensions(0, sw, sh, levels);
        float lod = log2(float(max(sw, 1u))) * roughness * saturate(g_params.ior * 2.0 - 2.0);
        float3 transmitted = g_scene_color.SampleLevel(g_scene_sampler, exit_uv, lod).rgb;
        transmitted *= albedo; // spec: transmitted light is tinted by base colour

        // Beer-Lambert absorption through the medium (KHR_materials_volume).
        if (g_params.attenuation.w > 0.0 && thickness_world > 0.0) {
            float3 sigma = -log(clamp(g_params.attenuation.rgb, 1e-3, 1.0)) /
                           g_params.attenuation.w;
            transmitted *= exp(-sigma * thickness_world);
        }

        // Environment reflection: split-sum IBL, same as the deferred pass.
        float3 R = reflect(-V, world_n);
        float3 prefiltered =
            g_prefiltered.SampleLevel(g_ibl_sampler, R, roughness * PREFILTER_MAX_LOD).rgb;
        float2 brdf = g_brdf_lut.SampleLevel(g_ibl_sampler, float2(ndotv, roughness), 0.0).rg;
        float3 spec_ibl = prefiltered * (F * brdf.x + brdf.y);

        // Sun: specular highlight only — the diffuse fraction is what transmits.
        float3 h = normalize(V + L);
        float ndotl = max(dot(world_n, L), 0.0);
        float a = max(roughness * roughness, 1e-3);
        float3 sun_spec = pbr_d_ggx(max(dot(world_n, h), 0.0), a) *
                          pbr_v_smith(ndotv, ndotl, a) *
                          pbr_f_schlick(max(dot(V, h), 0.0), f0) * ndotl *
                          pc.light_color.rgb * pc.light_color.w;

        // The non-transmitted fraction shades as regular dielectric diffuse.
        float3 irradiance = g_irradiance.SampleLevel(g_ibl_sampler, world_n, 0.0).rgb;
        float3 diffuse = irradiance * albedo * occ;
        float3 base_diffuse = lerp(diffuse, transmitted, g_params.transmission_factor);

        float3 color = (1.0 - F) * base_diffuse + spec_ibl + sun_spec + emissive;
        return float4(color, 1.0); // background already composited
    }

    float3 lit = pbr_surface_brdf(world_n, V, L, albedo, metallic, roughness) *
                 pc.light_color.rgb * pc.light_color.w;
    float3 ambient = pc.ambient.rgb * albedo * occ;
    return float4(lit + ambient + emissive, alpha);
}
