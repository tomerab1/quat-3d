#include "engine/asset/material_asset.hpp"

#include <utility>

namespace engine::asset {

std::expected<MaterialAsset, core::Error>
upload_material(const PbrMaterialParams& params, MaterialTextures textures,
                rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer) {
    auto buffer = rhi::upload_device_buffer(
        allocator, transfer, &params, sizeof(PbrMaterialParams),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    if (!buffer) return std::unexpected(buffer.error());

    MaterialAsset material;
    material.param_address = rhi::buffer_device_address(transfer.device, *buffer);
    material.param_buffer = std::move(*buffer);
    material.params = params;
    material.textures = std::move(textures);
    return material;
}

} // namespace engine::asset
