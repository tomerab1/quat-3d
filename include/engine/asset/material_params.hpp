#pragma once

// The scalar PBR material parameters (PbrMaterialParams) and the texture
// flag / slot enums shared by MaterialAsset, the material override component
// and the shaders. Graphics-API-free, so components.hpp (and the installed
// SDK headers, P8) can use them without the Vulkan headers; the GPU-owning
// MaterialAsset lives in material_asset.hpp.

#include <array>
#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

namespace engine::asset {

// Which optional textures a material binds. Mirrors the MATERIAL_HAS_* bits in
// material.slang — keep the two in sync.
enum MaterialTextureFlags : std::uint32_t {
    material_has_base_color         = 1u << 0,
    material_has_normal             = 1u << 1,
    material_has_metallic_roughness = 1u << 2,
    material_has_emissive           = 1u << 3,
    material_has_occlusion          = 1u << 4,
    material_blend                  = 1u << 5, // alpha-blended (glTF BLEND) -> forward pass
    material_transmission           = 1u << 6, // refractive glass (KHR_materials_transmission)
    material_double_sided           = 1u << 7, // glTF doubleSided -> disable back-face culling
    material_has_thickness          = 1u << 8, // KHR_materials_volume thicknessTexture (G channel)
    // glTF alphaMode MASK: base-colour alpha x factor is tested against
    // alpha_cutoff (discard below) in every pass that draws the surface.
    material_alpha_mask             = 1u << 9,
};

// Texture slot indices for the per-slot KHR_texture_transform arrays below.
// Order matches the MaterialTextures fields and the shader-side convention.
enum MaterialTextureSlot : std::size_t {
    material_slot_base_color = 0,
    material_slot_normal,
    material_slot_metallic_roughness,
    material_slot_emissive,
    material_slot_occlusion,
    material_slot_thickness,
    material_slot_count,
};

// GPU uniform-buffer layout, std140-compatible (272 bytes). Field order and
// padding mirror PbrMaterialParams in material.slang exactly.
struct PbrMaterialParams {
    glm::vec4     base_color_factor{1.0F, 1.0F, 1.0F, 1.0F};
    glm::vec4     emissive_factor{0.0F, 0.0F, 0.0F, 0.0F}; // xyz used; w is padding
    float         metallic_factor    = 1.0F;
    float         roughness_factor   = 1.0F;
    float         occlusion_strength = 1.0F;
    float         normal_scale       = 1.0F;
    float         alpha_cutoff       = 0.5F;
    std::uint32_t flags              = 0;
    float         transmission_factor = 0.0F; // KHR_materials_transmission
    float         ior                 = 1.5F; // KHR_materials_ior
    // KHR_materials_volume: rgb = attenuation colour, w = attenuation distance
    // (0 = no attenuation; the spec default of +inf is stored as 0).
    glm::vec4     attenuation{1.0F, 1.0F, 1.0F, 0.0F};
    float         thickness = 0.0F;           // KHR_materials_volume thicknessFactor
    // KHR_materials_clearcoat (factors only; coat textures are a later slice).
    // Together with pad they fill bytes 84..96 as three scalar floats — the
    // shader must mirror them as scalars (a slang float3 would be std140-aligned
    // to offset 96 and shift the arrays below).
    float         clearcoat_factor    = 0.0F;
    float         clearcoat_roughness = 0.0F;
    float         pad                 = 0.0F;
    // KHR_texture_transform, one affine UV transform per texture slot (indexed
    // by MaterialTextureSlot): uv' = M * uv + offset. Each uv_mat entry packs
    // the 2x2 rotation*scale matrix rows as (m00, m01, m10, m11); offsets are
    // packed two per vec4 (slot 2k -> xy, slot 2k+1 -> zw). Identity default.
    std::array<glm::vec4, material_slot_count> uv_mat{
        glm::vec4{1.0F, 0.0F, 0.0F, 1.0F}, glm::vec4{1.0F, 0.0F, 0.0F, 1.0F},
        glm::vec4{1.0F, 0.0F, 0.0F, 1.0F}, glm::vec4{1.0F, 0.0F, 0.0F, 1.0F},
        glm::vec4{1.0F, 0.0F, 0.0F, 1.0F}, glm::vec4{1.0F, 0.0F, 0.0F, 1.0F}};
    std::array<glm::vec4, material_slot_count / 2> uv_offset{};
    // Free parameters for an authored material shader (ROADMAP A6,
    // MaterialInput::params.shader_params); the built-in material ignores them.
    std::array<glm::vec4, 2> shader_params{};
};
static_assert(sizeof(PbrMaterialParams) == 272,
              "PbrMaterialParams must stay 272 bytes and std140-compatible to "
              "match material.slang");

} // namespace engine::asset
