// Render-graph barrier planning (audit A3/A4/A7).

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

using engine::rhi::PassType;
using engine::rhi::RenderGraph;
using engine::rhi::ResourceUsage;
using engine::rhi::TransientImagePool;

constexpr VkExtent2D k_extent{32, 32};
constexpr VkImageUsageFlags k_usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

// A4: compute writes, a compute pass samples (barrier dst = COMPUTE), then a
// fragment pass samples in the same layout. The fragment read needs its own
// dependency on the write — it used to get none.
TEST(GpuRenderGraph, SecondReaderInANewStageGetsABarrier) {
    QUAT_REQUIRE_GPU(g);
    TransientImagePool pool(g->device, g->allocator);
    RenderGraph graph(pool);
    const auto img = graph.create_transient_image("hdr", VK_FORMAT_R16G16B16A16_SFLOAT, k_extent, k_usage);
    (void)graph.add_pass("write", PassType::compute).writes(img, ResourceUsage::storage_write);
    (void)graph.add_pass("read_compute", PassType::compute).reads(img, ResourceUsage::sampled_compute);
    (void)graph.add_pass("read_fragment", PassType::graphics).reads(img, ResourceUsage::sampled);
    (void)graph.add_pass("read_compute_again", PassType::compute)
        .reads(img, ResourceUsage::sampled_compute);
    ASSERT_OK(graph.compile());

    const auto frag = graph.planned_barriers("read_fragment");
    ASSERT_EQ(frag.size(), 1U);
    EXPECT_TRUE(frag[0].srcStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    EXPECT_TRUE(frag[0].srcAccessMask & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    EXPECT_TRUE(frag[0].dstStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    // A reader already covered by an earlier barrier needs none.
    EXPECT_TRUE(graph.planned_barriers("read_compute_again").empty());
}

// A7: a read-modify-write pass must make the previous write visible to its
// storage READ access, not only its write.
TEST(GpuRenderGraph, ReadModifyWriteSeesThePreviousWrite) {
    QUAT_REQUIRE_GPU(g);
    TransientImagePool pool(g->device, g->allocator);
    RenderGraph graph(pool);
    const auto img = graph.create_transient_image("hdr", VK_FORMAT_R16G16B16A16_SFLOAT, k_extent, k_usage);
    (void)graph.add_pass("light", PassType::compute).writes(img, ResourceUsage::storage_write);
    (void)graph.add_pass("composite", PassType::compute).writes(img, ResourceUsage::storage_read_write);
    ASSERT_OK(graph.compile());
    const auto b = graph.planned_barriers("composite");
    ASSERT_EQ(b.size(), 1U);
    EXPECT_TRUE(b[0].dstAccessMask & VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    EXPECT_TRUE(b[0].srcAccessMask & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

// Write-after-read: a write following reads waits for the readers' stages.
TEST(GpuRenderGraph, WriteAfterReadWaitsForTheReaders) {
    QUAT_REQUIRE_GPU(g);
    TransientImagePool pool(g->device, g->allocator);
    RenderGraph graph(pool);
    const auto img = graph.create_transient_image("img", VK_FORMAT_R8G8B8A8_UNORM, k_extent, k_usage);
    (void)graph.add_pass("w1", PassType::compute).writes(img, ResourceUsage::storage_write);
    (void)graph.add_pass("r", PassType::graphics).reads(img, ResourceUsage::sampled);
    (void)graph.add_pass("w2", PassType::compute).writes(img, ResourceUsage::storage_write);
    ASSERT_OK(graph.compile());
    const auto b = graph.planned_barriers("w2");
    ASSERT_EQ(b.size(), 1U);
    EXPECT_TRUE(b[0].srcStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
}

// A3: an imported image's first barrier waits on the stage it was made
// available at (the swapchain's acquire semaphore wait stage).
TEST(GpuRenderGraph, ImportedImageFirstBarrierWaitsOnItsAvailableStage) {
    QUAT_REQUIRE_GPU(g);
    TransientImagePool pool(g->device, g->allocator);
    auto backing = pool.acquire(VK_FORMAT_R8G8B8A8_UNORM, k_extent,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    ASSERT_OK(backing);
    RenderGraph graph(pool);
    const auto img = graph.import_image("swapchain", backing->image, backing->view,
                                        VK_FORMAT_R8G8B8A8_UNORM, k_extent,
                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    (void)graph.add_pass("copy", PassType::transfer).writes(img, ResourceUsage::transfer_dst);
    ASSERT_OK(graph.compile());
    const auto b = graph.planned_barriers("copy");
    ASSERT_EQ(b.size(), 1U);
    EXPECT_EQ(b[0].srcStageMask, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
}

} // namespace
