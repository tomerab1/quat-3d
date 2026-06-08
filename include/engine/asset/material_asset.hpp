#pragma once

// MaterialAsset — a glTF metallic-roughness PBR material (Phase 3, Slice 3.4).
//
// Owns a device-local uniform buffer holding the scalar parameters (mirroring
// PbrMaterialParams in assets/shaders/lib/material.slang) and ref-counted
// handles to its (optional) textures. The GBuffer pass (Phase 3.5) builds a
// descriptor buffer from `param_address` + the texture views/samplers; this
// slice defines the asset and its GPU upload only.

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
};

// GPU uniform-buffer layout, std140-compatible (64 bytes). Field order and
// padding mirror PbrMaterialParams in material.slang exactly.
struct PbrMaterialParams {
    glm::vec4     base_color_factor{1.0F, 1.0F, 1.0F, 1.0F};
    glm::vec4     emissive_factor{0.0F, 0.0F, 0.0F, 0.0F};
    float         metallic_factor    = 1.0F;
    float         roughness_factor   = 1.0F;
    float         occlusion_strength = 1.0F;
    float         normal_scale       = 1.0F;
    float         alpha_cutoff       = 0.5F;
    std::uint32_t flags              = 0;
    glm::vec2     pad{0.0F, 0.0F};
};
static_assert(sizeof(PbrMaterialParams) == 64,
              "PbrMaterialParams must stay 64 bytes and std140-compatible to "
              "match material.slang");

// The (optional) textures a material samples. A slot is a null handle when the
// corresponding MaterialTextureFlags bit is unset.
struct MaterialTextures {
    AssetHandle<TextureAsset> base_color;
    AssetHandle<TextureAsset> normal;
    AssetHandle<TextureAsset> metallic_roughness;
    AssetHandle<TextureAsset> emissive;
    AssetHandle<TextureAsset> occlusion;
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
