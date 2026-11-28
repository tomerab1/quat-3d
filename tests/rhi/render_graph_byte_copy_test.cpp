// Byte copies between render-graph images share one storage format.
//
// The player tonemaps into a UNORM image (the shader encodes sRGB itself) and
// copies it into the sRGB swapchain. As a UNORM -> sRGB vkCmdCopyImage that
// is valid Vulkan, but MoltenVK copies through a UNORM texture view of the
// swapchain image and caches it, made from whichever CAMetalDrawable was
// current, for the image's lifetime: later frames copied into a recycled
// drawable and vkDestroySwapchainKHR released the stale view — the packaged
// macOS player died with SIGSEGV at exit. The intermediate is now stored in
// the target's format and only *viewed* as UNORM, and rhi::add_byte_copy
// refuses copies that would reinterpret the bytes (on every platform, so the
// next one fails in CI, not in a macOS package).

#include <cstdint>
#include <cstdlib>
#include <string>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace {

using engine::rhi::add_byte_copy;
using engine::rhi::PassType;
using engine::rhi::RenderGraph;
using engine::rhi::ResourceUsage;
using engine::rhi::TransientImagePool;

constexpr VkExtent2D k_extent{16, 8};
constexpr VkImageUsageFlags k_ldr_usage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

TEST(RenderGraphByteCopy, RefusesToReinterpretTheStorageFormat) {
    RenderGraph graph; // declarations only: nothing is compiled
    const auto swapchain =
        graph.import_image("swapchain", VK_NULL_HANDLE, VK_NULL_HANDLE, VK_FORMAT_B8G8R8A8_SRGB,
                           k_extent, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    EXPECT_EQ(graph.storage_format(swapchain), VK_FORMAT_B8G8R8A8_SRGB);

    const auto unorm =
        graph.create_transient_image("ldr", VK_FORMAT_B8G8R8A8_UNORM, k_extent, k_ldr_usage);
    EXPECT_EQ(graph.storage_format(unorm), VK_FORMAT_B8G8R8A8_UNORM);
    const auto refused = add_byte_copy(graph, "present_copy", unorm, swapchain, k_extent);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message.find("present_copy"), std::string::npos)
        << refused.error().message;
    EXPECT_FALSE(add_byte_copy(graph, "load", swapchain, unorm, k_extent).has_value());

    const auto stored = graph.create_transient_image("ldr", VK_FORMAT_B8G8R8A8_UNORM, k_extent,
                                                     k_ldr_usage, VK_FORMAT_B8G8R8A8_SRGB);
    EXPECT_EQ(graph.storage_format(stored), VK_FORMAT_B8G8R8A8_SRGB);
    EXPECT_EQ(graph.binding(stored).format, VK_FORMAT_B8G8R8A8_UNORM) << "passes see the view";
    EXPECT_OK(add_byte_copy(graph, "present_copy", stored, swapchain, k_extent));
    EXPECT_OK(add_byte_copy(graph, "load", swapchain, stored, k_extent));
}

// The tonemap's path in miniature: render 0.5 through the UNORM view of an
// image stored as sRGB, byte-copy it into an sRGB image and read the bytes.
// An sRGB view would have encoded 0.5 as 188; the stored byte must be 128.
TEST(GpuRenderGraphByteCopy, UnormViewOfSrgbStorageKeepsTheBytes) {
    QUAT_REQUIRE_GPU(g);
    auto target = g->allocator.create_image(VK_FORMAT_R8G8B8A8_SRGB, k_extent,
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    ASSERT_OK(target);
    TransientImagePool pool(g->device, g->allocator);
    RenderGraph graph(pool);
    const auto dst = graph.import_image("target", target->handle(), VK_NULL_HANDLE,
                                        VK_FORMAT_R8G8B8A8_SRGB, k_extent,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const auto ldr = graph.create_transient_image("ldr", VK_FORMAT_R8G8B8A8_UNORM, k_extent,
                                                  k_ldr_usage, graph.storage_format(dst));
    (void)graph.add_pass("tonemap", PassType::graphics)
        .writes(ldr, ResourceUsage::color_attachment)
        .execute([ldr](engine::rhi::PassContext& ctx) {
            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = ctx.resolve(ldr).view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.5F, 0.25F, 0.75F, 1.0F}};
            VkRenderingInfo info{};
            info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            info.renderArea = {{0, 0}, k_extent};
            info.layerCount = 1;
            info.colorAttachmentCount = 1;
            info.pColorAttachments = &color;
            vkCmdBeginRendering(ctx.cmd(), &info);
            vkCmdEndRendering(ctx.cmd());
        });
    ASSERT_OK(add_byte_copy(graph, "present_copy", ldr, dst, k_extent));
    auto host = quat::test::render_to_host(*g, graph, dst, k_extent, 4);
    ASSERT_OK(host);
    const auto px = host->rgba8(k_extent.width / 2, k_extent.height / 2);
    EXPECT_LE(std::abs(px.r - 128), 1) << px.r;
    EXPECT_LE(std::abs(px.g - 64), 1) << px.g;
    EXPECT_LE(std::abs(px.b - 191), 1) << px.b;
    EXPECT_EQ(px.a, 255);
}

TEST(GpuTransientPool, StorageFormatIsPartOfTheKey) {
    QUAT_REQUIRE_GPU(g);
    TransientImagePool pool(g->device, g->allocator);
    for (int frame = 0; frame < 3; ++frame) {
        ASSERT_OK(pool.acquire(VK_FORMAT_R8G8B8A8_UNORM, k_extent, k_ldr_usage));
        ASSERT_OK(pool.acquire(VK_FORMAT_R8G8B8A8_UNORM, k_extent, k_ldr_usage,
                               VK_FORMAT_R8G8B8A8_SRGB));
        pool.reset();
    }
    EXPECT_EQ(pool.size(), 2U) << "UNORM storage and sRGB storage are different images";
}

TEST(GpuTransientPool, StorageFormatMustBeTheSrgbUnormTwin) {
    QUAT_REQUIRE_GPU(g);
    TransientImagePool pool(g->device, g->allocator);
    ASSERT_ERR(pool.acquire(VK_FORMAT_R8G8B8A8_UNORM, k_extent, k_ldr_usage,
                            VK_FORMAT_B8G8R8A8_SRGB));
    EXPECT_EQ(pool.size(), 0U);
}

// sRGB formats support no storage-image usage, and usage is validated against
// the image's own (storage) format: refused with a precise error up front.
TEST(GpuTransientPool, StorageUsageOnSrgbStorageIsRefused) {
    QUAT_REQUIRE_GPU(g);
    TransientImagePool pool(g->device, g->allocator);
    const auto refused =
        pool.acquire(VK_FORMAT_R8G8B8A8_UNORM, k_extent, VK_IMAGE_USAGE_STORAGE_BIT,
                     VK_FORMAT_R8G8B8A8_SRGB);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message.find("storage usage"), std::string::npos)
        << refused.error().message;
    EXPECT_EQ(pool.size(), 0U);
    ASSERT_OK(pool.acquire(VK_FORMAT_R8G8B8A8_UNORM, k_extent, VK_IMAGE_USAGE_STORAGE_BIT));
}

} // namespace
