// TerrainPass: the layered material (R26.1) — material constants, texture
// sets, per-tile attributes and the tile descriptors that bind them.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "engine/renderer/terrain_pass.hpp"
#include "engine/rhi/device.hpp"

namespace engine::renderer {

namespace {

constexpr int k_retire_frames = 3; // outlive every frame in flight

// G19 terrain edits: no paint, full grass (r g b = 0, a = 255).
constexpr std::uint32_t k_unpainted = 0xFF000000U;
// Flat (concavity 0 -> 128) and dry (flow 0).
constexpr std::uint32_t k_flat_dry = 0xFF000080U;

} // namespace

std::expected<void, core::Error>
TerrainPass::create_material_resources(const rhi::TransferContext& transfer) {
    const auto texel = [&](std::uint32_t value, rhi::GpuImage& image,
                           rhi::ImageView& view) -> std::expected<void, core::Error> {
        auto img = rhi::upload_device_image(*allocator_, transfer, &value, sizeof(value), {1, 1},
                                            VK_FORMAT_R8G8B8A8_UNORM);
        if (!img) return std::unexpected(img.error());
        image = std::move(*img);
        auto v = rhi::create_image_view(device_->handle(), image.handle(), VK_FORMAT_R8G8B8A8_UNORM);
        if (!v) return std::unexpected(v.error());
        view = std::move(*v);
        return {};
    };
    if (auto r = texel(k_unpainted, default_paint_, default_paint_view_); !r) return r;
    if (auto r = texel(k_flat_dry, default_attributes_, default_attributes_view_); !r) return r;

    auto textures = TerrainTextures::create(*allocator_, transfer);
    if (!textures) return std::unexpected(textures.error());
    textures_ = std::move(*textures);
    material_ = terrain::default_terrain_material();
    refresh_material(true);
    return {};
}

void TerrainPass::set_material(const terrain::TerrainMaterial& material) {
    if (!textures_) return;
    // Cheap test first: the packed constants of the new material with the
    // slices the current arrays give (also catches set-list changes).
    std::vector<std::string> sets;
    sets.reserve(material.layers.size());
    for (const terrain::MaterialLayer& l : material.layers) sets.push_back(l.texture_set);
    bool same = material_set_ && sets.size() == material_.layers.size();
    for (std::size_t i = 0; same && i < sets.size(); ++i) {
        same = sets[i] == material_.layers[i].texture_set;
    }
    material_ = material;
    material_set_ = true;
    if (!same) textures_->request(sets);
    refresh_material(false);
}

void TerrainPass::refresh_material(bool force) {
    if (!textures_) return;
    std::vector<int> slices;
    slices.reserve(material_.layers.size());
    for (const terrain::MaterialLayer& l : material_.layers) {
        slices.push_back(textures_->slice_of(l.texture_set));
    }
    const TerrainMaterialGpu packed =
        pack_terrain_material(material_, slices, textures_->mean_mip());
    if (!force && material_buffer_.handle() != VK_NULL_HANDLE &&
        std::memcmp(&packed, &material_gpu_, sizeof(packed)) == 0) {
        return;
    }
    material_gpu_ = packed;
    auto buffer = allocator_->create_buffer(
        sizeof(TerrainMaterialGpu),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!buffer) {
        std::fprintf(stderr, "[terrain] material buffer: %s\n", buffer.error().message.c_str());
        return;
    }
    std::memcpy(buffer->mapped(), &material_gpu_, sizeof(material_gpu_));
    if (auto flushed = buffer->flush(); !flushed) {
        std::fprintf(stderr, "[terrain] material buffer: %s\n", flushed.error().message.c_str());
        return;
    }
    if (material_buffer_.handle() != VK_NULL_HANDLE) {
        retired_buffers_.emplace_back(k_retire_frames, std::move(material_buffer_));
    }
    material_buffer_ = std::move(*buffer);
    rewrite_descriptors();
}

std::expected<rhi::DescriptorBuffer, core::Error>
TerrainPass::make_tile_descriptor(const Tile& tile) const {
    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    db->write_sampled_image(0, tile.view.handle(), ro);
    db->write_sampler(1, sampler_.handle());
    db->write_sampled_image(2, default_paint_view_.handle(), ro);
    db->write_sampled_image(3,
                            tile.attributes_view.handle() != VK_NULL_HANDLE
                                ? tile.attributes_view.handle()
                                : default_attributes_view_.handle(),
                            ro);
    db->write_uniform_buffer(4, rhi::buffer_device_address(device_->handle(), material_buffer_),
                             sizeof(TerrainMaterialGpu));
    db->write_sampled_image(5, textures_->albedo_view(), ro);
    db->write_sampled_image(6, textures_->normal_view(), ro);
    db->write_sampled_image(7, textures_->packed_view(), ro);
    db->write_sampler(8, textures_->sampler());
    return std::move(*db);
}

void TerrainPass::rewrite_descriptors() {
    for (auto& [coord, tile] : tiles_) {
        auto db = make_tile_descriptor(tile);
        if (!db) {
            std::fprintf(stderr, "[terrain] descriptor: %s\n", db.error().message.c_str());
            continue;
        }
        retired_descriptors_.emplace_back(k_retire_frames, std::move(tile.descriptor));
        tile.descriptor = std::move(*db);
    }
}

void TerrainPass::retire_tile(Tile&& tile) {
    attributes_.cancel(tile.attributes.handle());
    retired_.emplace_back(k_retire_frames, std::move(tile));
}

void TerrainPass::wait_for_textures() {
    if (textures_ && textures_->wait()) refresh_material(true);
}

} // namespace engine::renderer
