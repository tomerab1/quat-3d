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
//     and alpha with MIN). The lighting pass tints receivers behind that depth,
//     tap by tap inside the depth PCF / PCSS kernel (lib/csm.slang
//     sample_cascade).
// Per surface the light crosses, a blended caster of alpha a lets (1 - a)
// through, and a transmissive one transmission x (1 - F_avg) x base colour: a
// closed glass object has two such interfaces on every light path. F_avg is
// the cosine-weighted hemispherical average of the dielectric Fresnel term,
// not the Fresnel at the light's angle: the shadow map does not model
// refraction, so the light a curved surface reflects at grazing angles is not
// balanced by the light it bends toward the centre (the caustic). With the
// angle-dependent term the silhouette texels of a glass sphere went black and
// drew a dark rim around its shadow (issue #77). 1 - F_avg is exactly the
// fraction a sphere's interface transmits over its projected area, so the
// shadow still lets the right amount of light through overall. Beer-Lambert volume absorption belongs
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

// Cosine-weighted hemispherical average of the dielectric Fresnel
// reflectance for relative index `ior` (d'Eon & Irving 2011 fit; 0.09 at
// 1.5). Light crossing from the denser side reflects the same average.
float average_fresnel(float ior) {
    float eta = max(ior, 1.0 / max(ior, 1e-3));
    return (eta - 1.0) / (4.08567 + 1.00071 * eta);
}

// Light one translucent surface lets through (rgb), given its base colour and
// the volume thickness to absorb over (0 on faces where the light leaves the
// volume).
float3 surface_transmittance(float4 base, float thickness_world) {
    float opacity = (g_params.flags & MATERIAL_BLEND) != 0 ? base.a : 1.0;
    float3 through = float3(0.0, 0.0, 0.0);
    if ((g_params.flags & MATERIAL_TRANSMISSION) != 0) {
        float fresnel = average_fresnel(g_params.ior);
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
    // Absorb once per volume: on the light's entry face only.
    float thickness_world =
        dot(n, pc.to_light.xyz) > 0.0 ? g_params.thickness * input.thickness_scale : 0.0;
    return float4(surface_transmittance(base, thickness_world), saturate(input.depth));
}
