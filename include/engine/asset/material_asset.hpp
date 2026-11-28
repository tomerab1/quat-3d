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
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_params.hpp"
#include "engine/asset/texture_asset.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::asset {

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
    // Authored material shader drawing this material (ROADMAP A6): the stem
    // of assets/shaders/materials/<shader>.slang. Empty = the built-in PBR
    // program. Only the GBuffer pass honours it (blended materials render
    // forward with the built-in shading).
    std::string       shader;

    [[nodiscard]] bool valid() const noexcept {
        return param_buffer.handle() != VK_NULL_HANDLE && param_address != 0;
    }
};

// Uploads `params` into a device-local uniform buffer (with device-address +
// transfer-dst usage) and assembles a MaterialAsset referencing `textures`.
[[nodiscard]] std::expected<MaterialAsset, core::Error>
upload_material(const PbrMaterialParams& params, MaterialTextures textures,
                rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer);

// A copy of `base` with `params` (per-entity material tuning, A8): every field
// but the parameter buffer is copied from `base` — keep it that way when
// MaterialAsset grows — and `params` is uploaded into a buffer of its own.
[[nodiscard]] std::expected<MaterialAsset, core::Error>
derive_material(const MaterialAsset& base, const PbrMaterialParams& params,
                rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer);

} // namespace engine::asset
