// The bloom pass on an HDR image holding one bright (8.0) 16x16 block on
// black: the block survives the read-modify-write composite and its glow
// spreads into pixels well outside it.

#include <cstdint>
#include <cstring>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/bloom_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_commands.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

namespace r = engine::renderer;
namespace rhi = engine::rhi;
using engine::core::Error;
using quat::test::HostImage;

constexpr VkExtent2D    k_extent{128, 128};
constexpr std::uint32_t k_block = 16;

std::expected<HostImage, Error> bloom_bright_block(quat::test::GpuContext& g) {
    auto bloom = r::BloomPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!bloom) return std::unexpected(bloom.error());
    auto hdr = g.allocator.create_image(r::hdr_color_format, k_extent,
                                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!hdr) return std::unexpected(hdr.error());
    auto view = rhi::create_image_view(g.device.handle(), hdr->handle(), r::hdr_color_format);
    if (!view) return std::unexpected(view.error());

    std::vector<std::uint16_t> px(static_cast<std::size_t>(k_block) * k_block * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = px[i + 1] = px[i + 2] = 0x4800; // half 8.0
        px[i + 3] = 0x3C00;                     // half 1.0
    }
    auto staging = g.allocator.create_staging_buffer(px.size() * sizeof(std::uint16_t));
    if (!staging) return std::unexpected(staging.error());
    std::memcpy(staging->mapped(), px.data(), px.size() * sizeof(std::uint16_t));

    // Clear to black, copy the block into the centre, hand it to the shaders.
    const VkImage image = hdr->handle();
    const VkBuffer src = staging->handle();
    const bool prepared = quat::test::submit_blocking(g.device, [&](VkCommandBuffer cmd) {
        quat::test::image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        quat::test::image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {static_cast<std::int32_t>(k_extent.width / 2 - k_block / 2),
                              static_cast<std::int32_t>(k_extent.height / 2 - k_block / 2), 0};
        region.imageExtent = {k_block, k_block, 1};
        vkCmdCopyBufferToImage(cmd, src, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        quat::test::image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    if (!prepared) return std::unexpected(Error{"bloom test: upload failed"});

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle handle = graph.import_image(
        "bloom_test_hdr", image, view->handle(), r::hdr_color_format, k_extent,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    r::BloomParams params;
    params.threshold = 1.0F;
    params.intensity = 1.0F;
    if (auto added = bloom->add_to_graph(graph, handle, k_extent, params); !added) {
        return std::unexpected(added.error());
    }
    return quat::test::render_to_host(g, graph, handle, k_extent, 8);
}

const std::expected<HostImage, Error>& bloomed(quat::test::GpuContext& g) {
    static const std::expected<HostImage, Error> image = bloom_bright_block(g);
    return image;
}

[[nodiscard]] float rgb_sum(const HostImage& img, std::uint32_t x, std::uint32_t y) {
    const glm::vec4 c = img.rgba16f(x, y);
    return c.r + c.g + c.b;
}

} // namespace

TEST(GpuBloom, BrightBlockSurvives) {
    QUAT_REQUIRE_GPU(g);
    const auto& img = bloomed(*g);
    ASSERT_OK(img);
    EXPECT_GT(rgb_sum(*img, 64, 64), 1.0F);
}

// (96, 64) lies well outside the block (x in [56, 72)) — black before bloom.
TEST(GpuBloom, HighlightGlowsIntoItsSurroundings) {
    QUAT_REQUIRE_GPU(g);
    const auto& img = bloomed(*g);
    ASSERT_OK(img);
    EXPECT_GT(rgb_sum(*img, 96, 64), 0.003F);
}
