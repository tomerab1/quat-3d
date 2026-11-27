#include "engine/asset/material_asset.hpp"

#include <utility>

namespace engine::asset {

namespace {

// Uploads `material.params` into a new device-local uniform buffer of its own.
[[nodiscard]] std::expected<void, core::Error> upload_params(MaterialAsset& material,
                                                             rhi::GpuAllocator& allocator,
                                                             const rhi::TransferContext& transfer) {
    auto buffer = rhi::upload_device_buffer(
        allocator, transfer, &material.params, sizeof(PbrMaterialParams),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    if (!buffer) return std::unexpected(buffer.error());
    material.param_address = rhi::buffer_device_address(transfer.device, *buffer);
    material.param_buffer = std::move(*buffer);
    return {};
}

} // namespace

std::expected<MaterialAsset, core::Error>
upload_material(const PbrMaterialParams& params, MaterialTextures textures,
                rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer) {
    MaterialAsset material;
    material.params = params;
    material.textures = std::move(textures);
    if (auto ok = upload_params(material, allocator, transfer); !ok) {
        return std::unexpected(ok.error());
    }
    return material;
}

std::expected<MaterialAsset, core::Error>
derive_material(const MaterialAsset& base, const PbrMaterialParams& params,
                rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer) {
    // Copy of the base: every field except the GPU parameter buffer, which is
    // the derived material's own. A field added to MaterialAsset is copied here.
    MaterialAsset derived;
    derived.textures = base.textures;
    // Then the overridden parameters, uploaded into the new buffer.
    derived.params = params;
    if (auto ok = upload_params(derived, allocator, transfer); !ok) {
        return std::unexpected(ok.error());
    }
    return derived;
}

} // namespace engine::asset
