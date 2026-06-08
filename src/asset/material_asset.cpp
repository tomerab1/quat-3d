#include "engine/asset/material_asset.hpp"

#include <string>
#include <utility>

namespace engine::asset {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

} // namespace

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

std::expected<void, core::Error>
run_material_asset_self_test(rhi::GpuAllocator& allocator, const rhi::TransferContext& transfer) {
    PbrMaterialParams params;
    params.base_color_factor = {0.8F, 0.4F, 0.2F, 1.0F};
    params.emissive_factor = {0.1F, 0.0F, 0.0F, 0.0F};
    params.metallic_factor = 0.25F;
    params.roughness_factor = 0.6F;
    params.alpha_cutoff = 0.33F;
    params.flags = material_has_base_color | material_has_metallic_roughness;

    auto material = upload_material(params, MaterialTextures{}, allocator, transfer);
    if (!material) return std::unexpected(material.error());
    if (!material->valid()) {
        return fail("material self-test: asset not valid (buffer/address)");
    }
    if (material->param_buffer.size() != sizeof(PbrMaterialParams)) {
        return fail("material self-test: param buffer is not 64 bytes");
    }
    if (material->params.flags != (material_has_base_color | material_has_metallic_roughness)) {
        return fail("material self-test: flags were not preserved");
    }
    if (material->params.base_color_factor != params.base_color_factor ||
        material->params.metallic_factor != params.metallic_factor) {
        return fail("material self-test: params were not preserved");
    }
    // Empty MaterialTextures must leave every slot a null handle.
    if (material->textures.base_color.valid() || material->textures.normal.valid()) {
        return fail("material self-test: unset texture slots should be null handles");
    }
    return {};
}

} // namespace engine::asset
