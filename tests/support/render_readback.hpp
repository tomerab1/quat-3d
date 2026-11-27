#pragma once

// Render-and-read-back helpers for GPU pass tests:
//
//   auto mesh = quat::test::upload_mesh(*g, verts, indices);        // MeshAsset
//   ... build a RenderGraph whose output is `image` ...
//   auto px = quat::test::render_to_host(*g, graph, image, extent, 8); // RGBA16F
//   glm::vec4 c = px->rgba16f(x, y);
//
//   auto texels = quat::test::read_rgba16f(*g, lut.handle(), mip, layer, w, h);
//
// render_to_host appends a transfer pass to the caller's graph (the graph
// plans the layout transition), compiles, submits and waits, then copies the
// pixels out. read_rgba16f reads a persistent image that a pass left in
// SHADER_READ_ONLY_OPTIMAL and restores that layout afterwards.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <vulkan/vulkan.h>

#include "engine/asset/mesh_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/gpu_commands.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace quat::test {

// Uploads an indexed triangle list as a single-submesh MeshAsset.
[[nodiscard]] inline std::expected<engine::asset::MeshAsset, engine::core::Error>
upload_mesh(GpuContext& g, std::span<const engine::asset::Vertex> vertices,
            std::span<const std::uint32_t> indices) {
    auto vb = engine::rhi::upload_device_buffer(g.allocator, g.transfer, vertices.data(),
                                                vertices.size_bytes(),
                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vb) return std::unexpected(vb.error());
    auto ib = engine::rhi::upload_device_buffer(g.allocator, g.transfer, indices.data(),
                                                indices.size_bytes(),
                                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!ib) return std::unexpected(ib.error());
    engine::asset::MeshAsset mesh;
    mesh.vertex_buffer = std::move(*vb);
    mesh.index_buffer = std::move(*ib);
    mesh.vertex_count = static_cast<std::uint32_t>(vertices.size());
    mesh.index_count = static_cast<std::uint32_t>(indices.size());
    mesh.submeshes = {engine::asset::SubMesh{0, mesh.index_count, 0}};
    return mesh;
}

// Host-visible, mapped buffer a copy can land in.
[[nodiscard]] inline std::expected<engine::rhi::GpuBuffer, engine::core::Error>
host_readback_buffer(engine::rhi::GpuAllocator& allocator, VkDeviceSize bytes) {
    return allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

// Tightly packed pixels of one image, row-major.
struct HostImage {
    VkExtent2D                extent{};
    std::uint32_t             bytes_per_pixel = 0;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] const std::uint8_t* at(std::uint32_t x, std::uint32_t y) const {
        return bytes.data() + (static_cast<std::size_t>(y) * extent.width + x) * bytes_per_pixel;
    }
    // RGBA8 (UNORM) pixel as 0..255 integers.
    [[nodiscard]] glm::ivec4 rgba8(std::uint32_t x, std::uint32_t y) const {
        const std::uint8_t* p = at(x, y);
        return {p[0], p[1], p[2], p[3]};
    }
    // RGBA16F pixel.
    [[nodiscard]] glm::vec4 rgba16f(std::uint32_t x, std::uint32_t y) const {
        std::uint16_t h[4];
        std::memcpy(h, at(x, y), sizeof(h));
        return {glm::unpackHalf1x16(h[0]), glm::unpackHalf1x16(h[1]), glm::unpackHalf1x16(h[2]),
                glm::unpackHalf1x16(h[3])};
    }
    [[nodiscard]] glm::uvec2 centre() const { return {extent.width / 2, extent.height / 2}; }
};

// Appends a read-back of `image` (mip 0, layer 0, colour) to `graph`, compiles
// and runs it — after `before(cmd)` when given, recorded into the same command
// buffer ahead of the graph — and returns the pixels.
template <typename Before>
[[nodiscard]] std::expected<HostImage, engine::core::Error>
render_to_host(GpuContext& g, engine::rhi::RenderGraph& graph, engine::rhi::ResourceHandle image,
               VkExtent2D extent, std::uint32_t bytes_per_pixel, Before&& before) {
    const VkDeviceSize size =
        static_cast<VkDeviceSize>(extent.width) * extent.height * bytes_per_pixel;
    auto readback = host_readback_buffer(g.allocator, size);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer dst = readback->handle();
    (void)graph.add_pass("test_readback", engine::rhi::PassType::transfer)
        .reads(image, engine::rhi::ResourceUsage::transfer_src)
        .execute([=](engine::rhi::PassContext& ctx) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(ctx.cmd(), ctx.resolve(image).image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
        });
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    auto done = submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        std::forward<Before>(before)(cmd);
        graph.execute(cmd);
        transfer_to_host_barrier(cmd);
    });
    if (!done) return std::unexpected(done.error());
    if (auto inv = readback->invalidate_mapped(); !inv) return std::unexpected(inv.error());

    HostImage out;
    out.extent = extent;
    out.bytes_per_pixel = bytes_per_pixel;
    out.bytes.resize(static_cast<std::size_t>(size));
    std::memcpy(out.bytes.data(), readback->mapped(), out.bytes.size());
    return out;
}

[[nodiscard]] inline std::expected<HostImage, engine::core::Error>
render_to_host(GpuContext& g, engine::rhi::RenderGraph& graph, engine::rhi::ResourceHandle image,
               VkExtent2D extent, std::uint32_t bytes_per_pixel) {
    return render_to_host(g, graph, image, extent, bytes_per_pixel, [](VkCommandBuffer) {});
}

// Blocking copy of one (mip, layer) of an RGBA16F image that is idle in
// SHADER_READ_ONLY_OPTIMAL (a baked LUT or probe); the layout is restored.
// Returns w * h texels, row-major.
[[nodiscard]] inline std::expected<std::vector<glm::vec4>, engine::core::Error>
read_rgba16f(GpuContext& g, VkImage image, std::uint32_t mip, std::uint32_t layer,
             std::uint32_t w, std::uint32_t h) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 8;
    auto readback = host_readback_buffer(g.allocator, size);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer dst = readback->handle();
    auto done = submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);

        std::swap(b.oldLayout, b.newLayout);
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        vkCmdPipelineBarrier2(cmd, &dep);
        transfer_to_host_barrier(cmd);
    });
    if (!done) return std::unexpected(done.error());
    if (auto inv = readback->invalidate_mapped(); !inv) return std::unexpected(inv.error());

    const auto* halfs = static_cast<const std::uint16_t*>(readback->mapped());
    std::vector<glm::vec4> out(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = {glm::unpackHalf1x16(halfs[i * 4]), glm::unpackHalf1x16(halfs[i * 4 + 1]),
                  glm::unpackHalf1x16(halfs[i * 4 + 2]), glm::unpackHalf1x16(halfs[i * 4 + 3])};
    }
    return out;
}

} // namespace quat::test
