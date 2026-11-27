#pragma once

// Test helper for the R20 post-pass tests: a persistent, sampled image
// uploaded from host texels (left in SHADER_READ_ONLY_OPTIMAL) that a test
// imports into its render graph as a pass input.

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/gpu_context.hpp"

namespace quat::test {

struct PostImage {
    engine::rhi::GpuImage  image;
    engine::rhi::ImageView view;
    VkFormat               format = VK_FORMAT_UNDEFINED;
    VkExtent2D             extent{0, 0};

    // `name` must outlive the graph (a string literal).
    [[nodiscard]] engine::rhi::ResourceHandle import(engine::rhi::RenderGraph& graph,
                                                     const char* name) const {
        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return graph.import_image(name, image.handle(), view.handle(), format, extent, ro, ro);
    }
};

[[nodiscard]] inline std::expected<PostImage, engine::core::Error>
upload_post_image(GpuContext& g, VkFormat format, VkExtent2D extent, const void* data,
                  std::size_t bytes) {
    auto image = engine::rhi::upload_device_image(g.allocator, g.transfer, data, bytes, extent,
                                                  format);
    if (!image) return std::unexpected(image.error());
    auto view = engine::rhi::create_image_view(g.device.handle(), image->handle(), format);
    if (!view) return std::unexpected(view.error());
    return PostImage{std::move(*image), std::move(*view), format, extent};
}

// RGBA16F image of linear colours (row-major, width * height).
[[nodiscard]] inline std::expected<PostImage, engine::core::Error>
upload_rgba16f(GpuContext& g, VkExtent2D extent, std::span<const glm::vec4> texels) {
    std::vector<std::uint16_t> half(texels.size() * 4);
    for (std::size_t i = 0; i < texels.size(); ++i) {
        for (int k = 0; k < 4; ++k) half[i * 4 + static_cast<std::size_t>(k)] = glm::packHalf1x16(texels[i][k]);
    }
    return upload_post_image(g, VK_FORMAT_R16G16B16A16_SFLOAT, extent, half.data(),
                             half.size() * sizeof(std::uint16_t));
}

// RG16F image (velocity).
[[nodiscard]] inline std::expected<PostImage, engine::core::Error>
upload_rg16f(GpuContext& g, VkExtent2D extent, std::span<const glm::vec2> texels) {
    std::vector<std::uint16_t> half(texels.size() * 2);
    for (std::size_t i = 0; i < texels.size(); ++i) {
        half[i * 2] = glm::packHalf1x16(texels[i].x);
        half[i * 2 + 1] = glm::packHalf1x16(texels[i].y);
    }
    return upload_post_image(g, VK_FORMAT_R16G16_SFLOAT, extent, half.data(),
                             half.size() * sizeof(std::uint16_t));
}

// R32F image standing in for the depth target ([0, 1] depth values; the
// passes read .r with Load, like a depth view).
[[nodiscard]] inline std::expected<PostImage, engine::core::Error>
upload_r32f(GpuContext& g, VkExtent2D extent, std::span<const float> texels) {
    return upload_post_image(g, VK_FORMAT_R32_SFLOAT, extent, texels.data(), texels.size_bytes());
}

} // namespace quat::test
