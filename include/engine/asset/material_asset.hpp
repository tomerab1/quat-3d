#pragma once

// MaterialAsset — a glTF metallic-roughness PBR material (Phase 3, Slice 3.4).
//
// Owns a device-local uniform buffer holding the scalar parameters (mirroring
// PbrMaterialParams in assets/shaders/lib/material.slang) and ref-counted
// handles to its (optional) textures. The GBuffer pass (Phase 3.5) builds a
// descriptor buffer from `param_address` + the texture views/samplers; this
// slice defines the asset and its GPU upload only.

#include <array>
#include <cstdint>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/texture_asset.hpp"
#include "engine/rhi/gpu_allocator.hpp"

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

// GPU uniform-buffer layout, std140-compatible (240 bytes). Field order and
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
};
static_assert(sizeof(PbrMaterialParams) == 240,
              "PbrMaterialParams must stay 240 bytes and std140-compatible to "
              "match material.slang");

// The (optional) textures a material samples. A slot is a null handle when the
// corresponding MaterialTextureFlags bit is unset.
struct MaterialTextures {
    AssetHandle<TextureAsset> base_color;
    AssetHandle<TextureAsset> normal;
    AssetHandle<TextureAsset> metallic_roughness;
    AssetHandle<TextureAsset> emissive;
    AssetHandle<TextureAsset> occlusion;
    AssetHandle<TextureAsset> thickness; // KHR_materials_volume (linear, G channel)
};

// Move-only: owns the parameter uniform buffer. Texture handles are ref-counted
// (cheap to copy) and may be null.
struct MaterialAsset {
    rhi::GpuBuffer    param_buffer;          // uniform buffer holding `params`
    VkDeviceAddress   param_address = 0;     // device address of param_buffer
    PbrMaterialParams params{};
    MaterialTextures  textures;

    [[nodiscard]] bool valid() const noexcept {
        return param_buffer.handle() != VK_NULL_HANDLE && param_address != 0;
    }
};

// Uploads `params` into a device-local uniform buffer (with device-address +
// transfer-dst usage) and assembles a MaterialAsset referencing `textures`.
[[nodiscard]] std::expected<MaterialAsset, core::Error>
upload_material(const PbrMaterialParams& params, MaterialTextures textures,
                rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer);

// Verifies the std140 layout contract and a params round-trip upload.
[[nodiscard]] std::expected<void, core::Error>
run_material_asset_self_test(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer);

} // namespace engine::asset
