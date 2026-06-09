// material.slang — material model interface + glTF metallic-roughness PBR
// (Phase 3, Slice 3.4).
//
// Per CLAUDE.md, material variation is expressed with a Slang `interface` and
// `struct`s implementing it — never #ifdef soup. The GBuffer pass (Phase 3.5) is
// written against IMaterial; PbrMaterial is the first implementation. Which
// optional textures are bound is data-driven via the `flags` bitfield, so a
// single specialized shader covers every material permutation.
//
// This file has no entry point: it is a module, imported by passes via
// `import material;` and validated at build time into a .slang-module.

// Bits marking which optional textures are present. Mirrors
// engine::asset::MaterialTextureFlags on the C++ side — keep in sync.
static const uint MATERIAL_HAS_BASE_COLOR         = 1u << 0;
static const uint MATERIAL_HAS_NORMAL             = 1u << 1;
static const uint MATERIAL_HAS_METALLIC_ROUGHNESS = 1u << 2;
static const uint MATERIAL_HAS_EMISSIVE           = 1u << 3;
static const uint MATERIAL_HAS_OCCLUSION          = 1u << 4;

// Texture slot indices for the per-slot KHR_texture_transform arrays. Mirrors
// engine::asset::MaterialTextureSlot.
public static const uint MATERIAL_SLOT_BASE_COLOR         = 0;
public static const uint MATERIAL_SLOT_NORMAL             = 1;
public static const uint MATERIAL_SLOT_METALLIC_ROUGHNESS = 2;
public static const uint MATERIAL_SLOT_EMISSIVE           = 3;
public static const uint MATERIAL_SLOT_OCCLUSION          = 4;
public static const uint MATERIAL_SLOT_THICKNESS          = 5;

// Scalar material parameters. Layout mirrors engine::asset::PbrMaterialParams
// (std140, 240 bytes) exactly — keep the two definitions in sync.
public struct PbrMaterialParams {
    public float4 base_color_factor;
    public float4 emissive_factor;     // xyz used; w is padding
    public float  metallic_factor;
    public float  roughness_factor;
    public float  occlusion_strength;
    public float  normal_scale;
    public float  alpha_cutoff;
    public uint   flags;
    public float  transmission_factor; // KHR_materials_transmission
    public float  ior;                 // KHR_materials_ior
    public float4 attenuation;         // rgb = attenuation colour, w = distance (0 = none)
    public float  thickness;           // KHR_materials_volume thicknessFactor
    // Three scalar floats, NOT float3: std140 would align a float3 to offset 96,
    // desynchronising every later field from the C++ glm::vec3 at offset 84.
    public float  pad0;
    public float  pad1;
    public float  pad2;
    // KHR_texture_transform per texture slot: uv' = M * uv + offset. uv_mat
    // packs the 2x2 matrix rows as (m00, m01, m10, m11); offsets are packed two
    // per float4 (slot 2k -> xy, slot 2k+1 -> zw). Identity when absent.
    public float4 uv_mat[6];
    public float4 uv_offset[3];
}

// Applies the KHR_texture_transform of `slot` to `uv`.
public float2 material_transform_uv(PbrMaterialParams p, uint slot, float2 uv) {
    float4 m = p.uv_mat[slot];
    float4 packed = p.uv_offset[slot >> 1];
    float2 offset = ((slot & 1u) == 0u) ? packed.xy : packed.zw;
    return float2(m.x * uv.x + m.y * uv.y, m.z * uv.x + m.w * uv.y) + offset;
}

// Surface attributes produced at a shading point, consumed by the GBuffer pass.
public struct SurfaceSample {
    public float3 albedo;      // linear base colour
    public float3 normal_ts;   // tangent-space normal (unit length)
    public float  metallic;
    public float  roughness;
    public float  occlusion;
    public float3 emissive;
    public float  alpha;
}

public interface IMaterial {
    SurfaceSample evaluate(float2 uv);
}

// glTF metallic-roughness PBR. Parameters live in a constant buffer; the five
// optional glTF textures share one sampler. Absent textures are skipped via
// `flags`, so unbound descriptors are never sampled.
public struct PbrMaterial : IMaterial {
    public ConstantBuffer<PbrMaterialParams> params;
    public Texture2D    base_color_tex;
    public Texture2D    normal_tex;
    public Texture2D    metallic_roughness_tex;
    public Texture2D    emissive_tex;
    public Texture2D    occlusion_tex;
    public SamplerState samp;

    public SurfaceSample evaluate(float2 uv) {
        SurfaceSample s;

        float4 base = params.base_color_factor;
        if ((params.flags & MATERIAL_HAS_BASE_COLOR) != 0) {
            base *= base_color_tex.Sample(
                samp, material_transform_uv(params, MATERIAL_SLOT_BASE_COLOR, uv));
        }
        s.albedo = base.rgb;
        s.alpha  = base.a;

        float metallic  = params.metallic_factor;
        float roughness = params.roughness_factor;
        if ((params.flags & MATERIAL_HAS_METALLIC_ROUGHNESS) != 0) {
            // glTF packs roughness in G, metallic in B.
            float3 mr = metallic_roughness_tex.Sample(
                samp, material_transform_uv(params, MATERIAL_SLOT_METALLIC_ROUGHNESS, uv)).rgb;
            roughness *= mr.g;
            metallic  *= mr.b;
        }
        s.metallic  = metallic;
        s.roughness = roughness;

        float3 n = float3(0, 0, 1);
        if ((params.flags & MATERIAL_HAS_NORMAL) != 0) {
            // Reconstruct Z from XY instead of trusting the blue channel: equal
            // for well-formed maps, but keeps mip-filtered or degenerate texels
            // (e.g. pure black) from producing backward-facing normals.
            float2 nxy = normal_tex.Sample(
                             samp, material_transform_uv(params, MATERIAL_SLOT_NORMAL, uv)).xy *
                             2.0 - 1.0;
            nxy *= params.normal_scale;
            float nz = sqrt(saturate(1.0 - dot(nxy, nxy)));
            n = normalize(float3(nxy, max(nz, 1e-3)));
        }
        s.normal_ts = n;

        float occ = 1.0;
        if ((params.flags & MATERIAL_HAS_OCCLUSION) != 0) {
            occ = lerp(1.0,
                       occlusion_tex.Sample(
                           samp, material_transform_uv(params, MATERIAL_SLOT_OCCLUSION, uv)).r,
                       params.occlusion_strength);
        }
        s.occlusion = occ;

        float3 emissive = params.emissive_factor.rgb;
        if ((params.flags & MATERIAL_HAS_EMISSIVE) != 0) {
            emissive *= emissive_tex.Sample(
                samp, material_transform_uv(params, MATERIAL_SLOT_EMISSIVE, uv)).rgb;
        }
        s.emissive = emissive;

        return s;
    }
}
