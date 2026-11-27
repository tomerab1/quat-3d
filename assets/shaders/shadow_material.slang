// shadow_material.slang — cascaded shadow casters that read their material
// (ROADMAP R12.11; issues #35 and #23).
//
// shadow.slang draws opaque casters depth-only with no fragment work. The
// casters here sample their material's base colour:
//   * fragment_masked — glTF MASK casters into the depth cascades: the same
//     alpha test as the GBuffer (lib/alpha_test.slang), so leaf cut-outs let
//     the sun through;
//   * fragment_translucent — transmissive (glass) and alpha-blended casters
//     into the translucency map instead of the depth cascades: each surface
//     the light crosses multiplies its transmittance into rgb, alpha keeps the
//     nearest such surface's depth (the pipeline blends rgb multiplicatively
//     and alpha with MIN). The lighting pass tints receivers behind that depth
//     (lib/csm.slang csm_transmittance).
// Per surface the light crosses, a blended caster of alpha a lets (1 - a)
// through, and a transmissive one transmission x (1 - Fresnel) x base colour
// (the interface terms of transparent.slang): a closed glass object has two
// such interfaces on every light path. Beer-Lambert volume absorption belongs
// to the volume, not to an interface: KHR_materials_volume `thickness` is the
// whole medium under the surface, so it is applied once, on the face where
// the light enters (outward normal toward the sun), and a ray through a
// closed volume, which crosses one entering and one leaving face, is
// absorbed exactly once.

import alpha_test;
import csm;
import instancing;
import material;

// Mirrors engine::renderer::ShadowMaterialPushConstants (48 bytes).
struct PushConstants {
    CascadeData*  cascades;
    InstanceData* instances; // indexed by instance_index() (includes firstInstance)
    float4        to_light;  // xyz = unit direction toward the sun
    uint          cascade;
    uint          pad0;
    uint          pad1;
    uint          pad2;
}
[[vk::push_constant]] PushConstants pc;

ConstantBuffer<PbrMaterialParams> g_params;
Texture2D                         g_base_color;
SamplerState                      g_samp;

struct VsIn {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
}

struct VsOut {
    float4 clip    : SV_Position;
    float3 world_n : NORMAL;
    float2 uv      : TEXCOORD0;
    float  depth   : TEXCOORD1; // cascade depth (orthographic: w = 1)
    // Volume thickness scale of the node transform (KHR_materials_volume
    // thickness is in local units), as transparent.slang computes it.
    nointerpolation float thickness_scale : TEXCOORD2;
}

[shader("vertex")]
VsOut vertex_main(VsIn input) {
    InstanceData inst = pc.instances[instance_index()];
    VsOut o;
    o.clip = csm_mul(pc.cascades[0], pc.cascade, inst.world_position(input.position));
    o.depth = o.clip.z / o.clip.w;
    o.world_n = inst.world_normal(input.normal);
    o.uv = input.uv;
    o.thickness_scale =
        length(mul((float3x3)inst.model, float3(0.577350, 0.577350, 0.577350)));
    return o;
}

// Alpha-tested casters only need the uv (pipeline vertex input: position at
// location 0, uv at location 1).
struct MaskedVsIn {
    float3 position : POSITION;
    float2 uv       : TEXCOORD0;
}

struct MaskedVsOut {
    float4 clip : SV_Position;
    float2 uv   : TEXCOORD0;
}

[shader("vertex")]
MaskedVsOut vertex_masked(MaskedVsIn input) {
    InstanceData inst = pc.instances[instance_index()];
    MaskedVsOut o;
    o.clip = csm_mul(pc.cascades[0], pc.cascade, inst.world_position(input.position));
    o.uv = input.uv;
    return o;
}

// Depth-only alpha-tested caster: nothing is written but depth.
[shader("fragment")]
void fragment_masked(MaskedVsOut input) {
    float alpha = material_base_color(g_params, g_base_color, g_samp, input.uv).a;
    AlphaMasked::test(alpha, g_params.alpha_cutoff);
}

// Light one translucent surface lets through (rgb), given its base colour,
// |cos| of the angle between its normal and the sun, and the volume thickness
// to absorb over (0 on faces where the light leaves the volume).
float3 surface_transmittance(float4 base, float cos_l, float thickness_world) {
    float opacity = (g_params.flags & MATERIAL_BLEND) != 0 ? base.a : 1.0;
    float3 through = float3(0.0, 0.0, 0.0);
    if ((g_params.flags & MATERIAL_TRANSMISSION) != 0) {
        float f0 = (g_params.ior - 1.0) / (g_params.ior + 1.0);
        f0 *= f0;
        float fresnel = f0 + (1.0 - f0) * pow(saturate(1.0 - cos_l), 5.0);
        float3 tint = base.rgb;
        if (g_params.attenuation.w > 0.0 && thickness_world > 0.0) {
            float3 sigma = -log(clamp(g_params.attenuation.rgb, 1e-3, 1.0)) /
                           g_params.attenuation.w;
            tint *= exp(-sigma * thickness_world);
        }
        // Metals do not transmit; the rest of the surface is the diffuse /
        // non-transmitted fraction, which blocks the sun like opaque matter.
        through = tint * (g_params.transmission_factor * (1.0 - g_params.metallic_factor) *
                          (1.0 - fresnel));
    }
    return (1.0 - opacity) + opacity * through;
}

[shader("fragment")]
float4 fragment_translucent(VsOut input) : SV_Target0 {
    float4 base = material_base_color(g_params, g_base_color, g_samp, input.uv);
    if ((g_params.flags & MATERIAL_ALPHA_MASK) != 0) {
        AlphaMasked::test(base.a, g_params.alpha_cutoff);
    }
    float3 n = normalize(input.world_n);
    float n_dot_l = dot(n, pc.to_light.xyz);
    float cos_l = abs(n_dot_l);
    // Absorb once per volume: on the light's entry face only.
    float thickness_world = n_dot_l > 0.0 ? g_params.thickness * input.thickness_scale : 0.0;
    return float4(surface_transmittance(base, cos_l, thickness_world), saturate(input.depth));
}
