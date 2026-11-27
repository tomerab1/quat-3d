// transparent.slang — forward alpha-blend + glass transmission (Phase 7).
//
// Blends over the deferred HDR. Plain BLEND materials use SRC_ALPHA blending.
// Transmission materials (KHR_materials_transmission + _volume + _ior) refract
// the view ray through the medium: the exit point is projected back to screen
// space and the opaque scene colour is sampled from a pre-blurred mip chain
// (mip = f(roughness, ior)), tinted by Beer-Lambert volume attenuation, and
// combined with split-sum IBL reflection + the sun's specular highlight.
// Both paths are also lit by the clustered point/spot lights (R10.1): each
// fragment loops only over its froxel's list from the light_cull pass.
// Material textures are bound explicitly (b0..6) so the scene-colour snapshot
// (b7..8), IBL maps (b9..12) and clustered lights (b14..17) can share set 0.

import instancing;
import clustered_lights;
import pbr;
import hdr;

static const uint MAT_HAS_BASE_COLOR = 1u << 0;
static const uint MAT_HAS_NORMAL     = 1u << 1;
static const uint MAT_HAS_MR         = 1u << 2;
static const uint MAT_HAS_EMISSIVE   = 1u << 3;
static const uint MAT_HAS_OCCLUSION  = 1u << 4;
static const uint MAT_TRANSMISSION   = 1u << 6;
static const uint MAT_HAS_THICKNESS  = 1u << 8;

// Prefiltered specular cubemap mip count - 1. Must match ibl_pass.cpp.
static const float PREFILTER_MAX_LOD = 4.0;

// Mirrors engine::asset::PbrMaterialParams (std140, 272 bytes).
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
    // KHR_materials_clearcoat factors (not yet shaded in the forward pass) +
    // padding. Scalar floats, NOT a float3: std140 would align a float3 to
    // offset 96, desynchronising every later field from the C++ layout.
    float  clearcoat_factor;
    float  clearcoat_roughness;
    float  pad;
    // KHR_texture_transform per texture slot (0=base colour, 1=normal, 2=MR,
    // 3=emissive, 4=occlusion, 5=thickness): uv' = M * uv + offset. uv_mat
    // packs the 2x2 matrix rows as (m00, m01, m10, m11); offsets are packed
    // two per float4 (slot 2k -> xy, slot 2k+1 -> zw).
    float4 uv_mat[6];
    float4 uv_offset[3];
    float4 shader_params[2];    // material-shader parameters (unused here)
}

// Mirrors engine::renderer::TransparentPushConstants (144 bytes). Each
// back-to-front sorted object is a single-instance draw reading its transform
// through the instance buffer, like the GBuffer pass (R9.1).
struct PushConstants {
    float4x4      view_proj;
    InstanceData* instances;  // indexed by instance_index() (includes firstInstance)
    uint2         pad;
    float4   camera_pos;      // xyz, w = IBL intensity
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
// Clustered lights (light_cull.slang output; dims.w = 0 when there are none).
[[vk::binding(14)]] ConstantBuffer<ClusterParams> g_cluster_params;
[[vk::binding(15)]] StructuredBuffer<GpuLight>    g_lights;
[[vk::binding(16)]] StructuredBuffer<uint2>       g_cluster_grid;    // offset, count
[[vk::binding(17)]] StructuredBuffer<uint>        g_cluster_indices;

// Applies the KHR_texture_transform of texture slot `slot` to `uv`.
float2 transformed_uv(uint slot, float2 uv) {
    float4 m = g_params.uv_mat[slot];
    float4 packed = g_params.uv_offset[slot >> 1];
    float2 offset = ((slot & 1u) == 0u) ? packed.xy : packed.zw;
    return float2(m.x * uv.x + m.y * uv.y, m.z * uv.x + m.w * uv.y) + offset;
}

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
    // Volume thickness scale of the node transform (KHR_materials_volume
    // thickness is in local units): length of M applied to a unit diagonal.
    nointerpolation float thickness_scale : TEXCOORD1;
}

[shader("vertex")]
VsOut vertex_main(VsIn input) {
    InstanceData inst = pc.instances[instance_index()];
    VsOut o;
    float4 world = inst.world_position(input.position);
    o.world_pos = world.xyz;
    o.clip = mul(pc.view_proj, world);
    // Cofactor normal matrix (non-uniform scale) + mirrored tangent handedness.
    o.world_n = normalize(inst.world_normal(input.normal));
    float4 t = inst.world_tangent(input.tangent);
    o.world_t = float4(normalize(t.xyz), t.w);
    o.uv = input.uv;
    o.thickness_scale =
        length(mul((float3x3)inst.model, float3(0.577350, 0.577350, 0.577350)));
    return o;
}

// Schlick Fresnel with a roughness cap, for the view-dependent environment terms.
float3 fresnel_schlick_roughness(float ndotv, float3 f0, float roughness) {
    float3 fmax = max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), f0);
    return f0 + (fmax - f0) * pow(saturate(1.0 - ndotv), 5.0);
}

// Sum of the clustered lights at `world_pos`: full BRDF (diffuse + specular)
// or, for glass, the specular lobe only (its diffuse fraction transmits).
float3 clustered_lighting(float3 world_pos, float3 N, float3 V, float3 albedo, float metallic,
                          float roughness, float3 f0, bool specular_only) {
    float3 sum = float3(0.0, 0.0, 0.0);
    if (g_cluster_params.dims.w == 0) {
        return sum;
    }
    uint2 range = g_cluster_grid[cluster_index_of_world(g_cluster_params, world_pos)];
    float ndotv = max(dot(N, V), 1e-4);
    float a = pbr_punctual_alpha(roughness); // finite highlight (issue #45)
    for (uint k = 0; k < range.y; ++k) {
        GpuLight light = g_lights[g_cluster_indices[range.x + k]];
        float3 L;
        float3 radiance;
        if (!light_incident(light, world_pos, L, radiance)) {
            continue;
        }
        if (specular_only) {
            float3 h = normalize(V + L);
            float ndotl = max(dot(N, L), 0.0);
            sum += pbr_d_ggx(max(dot(N, h), 0.0), a) * pbr_v_smith(ndotv, ndotl, a) *
                   pbr_f_schlick(max(dot(V, h), 0.0), f0) * ndotl * radiance;
        } else {
            sum += pbr_surface_brdf(N, V, L, albedo, metallic, roughness) * radiance;
        }
    }
    return sum;
}

[shader("fragment")]
float4 fragment_main(VsOut input) : SV_Target0 {
    float4 base = g_params.base_color_factor;
    if ((g_params.flags & MAT_HAS_BASE_COLOR) != 0) {
        base *= g_base_color.Sample(g_samp, transformed_uv(0, input.uv));
    }
    float3 albedo = base.rgb;
    float  alpha = base.a;

    float metallic = g_params.metallic_factor;
    float roughness = g_params.roughness_factor;
    if ((g_params.flags & MAT_HAS_MR) != 0) {
        float3 mr = g_metallic_roughness.Sample(g_samp, transformed_uv(2, input.uv)).rgb;
        roughness *= mr.g;
        metallic *= mr.b;
    }
    float3 nts = float3(0, 0, 1);
    if ((g_params.flags & MAT_HAS_NORMAL) != 0) {
        // Reconstruct Z from XY (see material.slang): robust against degenerate
        // or mip-filtered texels that would otherwise flip the normal backwards.
        float2 nxy = g_normal.Sample(g_samp, transformed_uv(1, input.uv)).xy * 2.0 - 1.0;
        nxy *= g_params.normal_scale;
        float nz = sqrt(saturate(1.0 - dot(nxy, nxy)));
        nts = normalize(float3(nxy, max(nz, 1e-3)));
    }
    float occ = 1.0;
    if ((g_params.flags & MAT_HAS_OCCLUSION) != 0) {
        occ = lerp(1.0, g_occlusion.Sample(g_samp, transformed_uv(4, input.uv)).r,
                   g_params.occlusion_strength);
    }
    float3 emissive = g_params.emissive_factor.rgb;
    if ((g_params.flags & MAT_HAS_EMISSIVE) != 0) {
        emissive *= g_emissive.Sample(g_samp, transformed_uv(3, input.uv)).rgb;
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

    // Specular AA: widen roughness by sub-pixel normal variance (same as the
    // GBuffer pass) so glass highlights and reflections don't sparkle.
    roughness = pbr_specular_aa(world_n, roughness);

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
        float thickness_world = g_params.thickness * input.thickness_scale;
        if ((g_params.flags & MAT_HAS_THICKNESS) != 0) {
            thickness_world *= g_thickness.Sample(g_samp, transformed_uv(5, input.uv)).g;
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
        float3 spec_ibl = prefiltered * (F * brdf.x + brdf.y) * pc.camera_pos.w;

        // Sun: specular highlight only — the diffuse fraction is what transmits.
        float3 h = normalize(V + L);
        float ndotl = max(dot(world_n, L), 0.0);
        float a = pbr_punctual_alpha(roughness); // finite highlight (issue #45)
        float3 sun_spec = pbr_d_ggx(max(dot(world_n, h), 0.0), a) *
                          pbr_v_smith(ndotv, ndotl, a) *
                          pbr_f_schlick(max(dot(V, h), 0.0), f0) * ndotl *
                          pc.light_color.rgb * pc.light_color.w;

        // The non-transmitted fraction shades as regular dielectric diffuse.
        float3 irradiance =
            g_irradiance.SampleLevel(g_ibl_sampler, world_n, 0.0).rgb * pc.camera_pos.w;
        float3 diffuse = irradiance * albedo * occ;
        float3 base_diffuse = lerp(diffuse, transmitted, g_params.transmission_factor);

        float3 point_spec = clustered_lighting(input.world_pos, world_n, V, albedo, metallic,
                                               roughness, f0, true);

        float3 color = (1.0 - F) * base_diffuse + spec_ibl + sun_spec + point_spec + emissive;
        return float4(hdr_store(color), 1.0); // background already composited
    }

    float3 lit = pbr_surface_brdf(world_n, V, L, albedo, metallic, roughness) *
                 pc.light_color.rgb * pc.light_color.w;
    lit += clustered_lighting(input.world_pos, world_n, V, albedo, metallic, roughness,
                              float3(0.04, 0.04, 0.04), false);
    float3 ambient = pc.ambient.rgb * albedo * occ;
    return float4(hdr_store(lit + ambient + emissive), alpha);
}
