// HDR range and NaN guards in the post chain (issue #45): the bloom
// composite keeps a pixel already at the half-float maximum finite, and the
// bloom prefilter drops a non-finite texel (counting it) instead of
// spreading it through the mip chain.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/renderer/bloom_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/non_finite_counters.hpp"
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

constexpr VkExtent2D    k_extent{64, 64};
constexpr std::uint16_t k_half_max = 0x7BFF; // 65504
constexpr std::uint16_t k_half_nan = 0x7E00;
constexpr std::uint16_t k_half_one = 0x3C00;

// Runs bloom (intensity 1) over a black HDR image with a `block` x `block`
// square of `value` (all three channels) at the centre; returns the HDR
// image after the composite. `counter` (0 = off) is the prefilter's
// non-finite counter.
std::expected<HostImage, Error> bloom_over(quat::test::GpuContext& g, std::uint16_t value,
                                           std::uint32_t block, VkDeviceAddress counter) {
    auto bloom = r::BloomPass::create(g.device, g.allocator, g.cache, g.shader_dir);
    if (!bloom) return std::unexpected(bloom.error());
    bloom->set_non_finite_counter(counter);
    auto hdr = g.allocator.create_image(r::hdr_color_format, k_extent,
                                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!hdr) return std::unexpected(hdr.error());
    auto view = rhi::create_image_view(g.device.handle(), hdr->handle(), r::hdr_color_format);
    if (!view) return std::unexpected(view.error());

    std::vector<std::uint16_t> px(static_cast<std::size_t>(block) * block * 4);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i] = px[i + 1] = px[i + 2] = value;
        px[i + 3] = k_half_one;
    }
    auto staging = g.allocator.create_staging_buffer(px.size() * sizeof(std::uint16_t));
    if (!staging) return std::unexpected(staging.error());
    std::memcpy(staging->mapped(), px.data(), px.size() * sizeof(std::uint16_t));

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
        region.imageOffset = {static_cast<std::int32_t>(k_extent.width / 2 - block / 2),
                              static_cast<std::int32_t>(k_extent.height / 2 - block / 2), 0};
        region.imageExtent = {block, block, 1};
        vkCmdCopyBufferToImage(cmd, src, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        quat::test::image_barrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    if (!prepared) return std::unexpected(Error{"hdr guard test: upload failed"});

    rhi::TransientImagePool pool(g.device, g.allocator);
    rhi::RenderGraph graph(pool);
    const rhi::ResourceHandle handle = graph.import_image(
        "hdr_guard_test_hdr", image, view->handle(), r::hdr_color_format, k_extent,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    r::BloomParams params;
    params.threshold = 1.0F;
    params.intensity = 1.0F;
    if (auto added = bloom->add_to_graph(graph, handle, k_extent, params); !added) {
        return std::unexpected(added.error());
    }
    return quat::test::render_to_host(g, graph, handle, k_extent, 8);
}

} // namespace

// A pixel at the half-float maximum plus its own bloom must not round to
// +Inf in the RGBA16F target (bloom_composite clamps with lib/hdr.slang).
TEST(GpuHdrGuard, BloomCompositeOverHalfMaxStaysFinite) {
    QUAT_REQUIRE_GPU(g);
    auto img = bloom_over(*g, k_half_max, 8, 0);
    ASSERT_OK(img);
    for (std::uint32_t y = 0; y < k_extent.height; ++y) {
        for (std::uint32_t x = 0; x < k_extent.width; ++x) {
            const glm::vec4 c = img->rgba16f(x, y);
            for (int k = 0; k < 3; ++k) {
                ASSERT_TRUE(std::isfinite(c[k])) << x << "," << y;
                ASSERT_LE(c[k], 65504.0F) << x << "," << y;
            }
        }
    }
    EXPECT_FLOAT_EQ(img->rgba16f(k_extent.width / 2, k_extent.height / 2).r, 65504.0F);
}

// One NaN texel: the prefilter drops it (and counts it), so every other pixel
// stays finite after the composite — no NaN block (the NaN texel itself is
// the lighting bug and stays as it was).
TEST(GpuHdrGuard, BloomPrefilterDropsANanTexel) {
    QUAT_REQUIRE_GPU(g);
    auto counters = r::NonFiniteCounters::create(g->device, g->allocator, 1);
    ASSERT_OK(counters);
    auto img = bloom_over(*g, k_half_nan, 1,
                          counters->address(0, r::NonFiniteSource::bloom_prefilter));
    ASSERT_OK(img);
    const std::uint32_t nx = k_extent.width / 2;
    const std::uint32_t ny = k_extent.height / 2;
    for (std::uint32_t y = 0; y < k_extent.height; ++y) {
        for (std::uint32_t x = 0; x < k_extent.width; ++x) {
            if (x == nx && y == ny) continue;
            const glm::vec4 c = img->rgba16f(x, y);
            for (int k = 0; k < 3; ++k) {
                ASSERT_TRUE(std::isfinite(c[k])) << "NaN spread to " << x << "," << y;
            }
        }
    }
    const auto counts = counters->collect(0);
    EXPECT_GE(counts[static_cast<std::size_t>(r::NonFiniteSource::bloom_prefilter)], 1U);
    EXPECT_EQ(counters->collect(0)[0], 0U) << "collect resets the counters";
}

// Each counter starts its own storage-buffer descriptor (the shaders bind it
// as RWStructuredBuffer<uint>, not through a device-address pointer): every
// address is aligned to minStorageBufferOffsetAlignment and distinct, and a
// count lands in its own slot and source only.
TEST(GpuHdrGuard, NonFiniteCountersAreSeparateDescriptorAlignedSlots) {
    QUAT_REQUIRE_GPU(g);
    constexpr std::uint32_t k_slots = 2;
    auto counters = r::NonFiniteCounters::create(g->device, g->allocator, k_slots);
    ASSERT_OK(counters);
    const VkDeviceSize align = g->device.properties().limits.minStorageBufferOffsetAlignment;
    std::vector<VkDeviceAddress> seen;
    for (std::uint32_t slot = 0; slot < k_slots; ++slot) {
        for (std::uint32_t s = 0; s < r::NonFiniteCounters::k_sources; ++s) {
            const VkDeviceAddress a = counters->address(slot, static_cast<r::NonFiniteSource>(s));
            ASSERT_NE(a, 0U);
            EXPECT_EQ(a % align, 0U) << "slot " << slot << " source " << s;
            for (const VkDeviceAddress b : seen) {
                EXPECT_GE(a > b ? a - b : b - a, r::NonFiniteCounters::k_counter_range);
            }
            seen.push_back(a);
        }
    }
    EXPECT_EQ(counters->address(k_slots, r::NonFiniteSource::taa_history), 0U);

    auto img = bloom_over(*g, k_half_nan, 1,
                          counters->address(1, r::NonFiniteSource::bloom_prefilter));
    ASSERT_OK(img);
    const auto slot0 = counters->collect(0);
    const auto slot1 = counters->collect(1);
    for (const std::uint32_t c : slot0) EXPECT_EQ(c, 0U) << "slot 0 was not bound";
    EXPECT_GE(slot1[static_cast<std::size_t>(r::NonFiniteSource::bloom_prefilter)], 1U);
    EXPECT_EQ(slot1[static_cast<std::size_t>(r::NonFiniteSource::taa_history)], 0U);
}
