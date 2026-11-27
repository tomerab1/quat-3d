// Render-graph barrier planning (audit A3/A4/A7).

#include <bit>
#include <cstdint>

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

// ---------------------------------------------------------------------------
// Buffer resources (R10.1). Imported buffers need no device or pool: barrier
// planning is pure bookkeeping, so these run as CPU tests with fake handles.
// ---------------------------------------------------------------------------

[[nodiscard]] VkBuffer fake_buffer(std::uintptr_t id) { return std::bit_cast<VkBuffer>(id); }

// Fill (transfer) -> compute read/write (atomics) -> compute read -> fragment
// read: each RAW hop gets exactly the barrier it needs, a reader already
// covered gets none, and the fragment reader gets its own (A4 for buffers).
TEST(RenderGraphBuffers, ReadAfterWriteBarriersFollowTheStages) {
    RenderGraph graph;
    const auto buf = graph.import_buffer("clusters", fake_buffer(0x10), 256);
    (void)graph.add_pass("reset", PassType::transfer).writes(buf, ResourceUsage::transfer_dst);
    (void)graph.add_pass("cull", PassType::compute).writes(buf, ResourceUsage::buffer_write_compute);
    (void)graph.add_pass("light", PassType::compute).reads(buf, ResourceUsage::buffer_read_compute);
    (void)graph.add_pass("light_again", PassType::compute)
        .reads(buf, ResourceUsage::buffer_read_compute);
    (void)graph.add_pass("forward", PassType::graphics)
        .reads(buf, ResourceUsage::buffer_read_fragment);
    ASSERT_OK(graph.compile());

    // First access of an idle buffer: nothing to wait for.
    EXPECT_TRUE(graph.planned_buffer_barriers("reset").empty());

    const auto cull = graph.planned_buffer_barriers("cull");
    ASSERT_EQ(cull.size(), 1U);
    EXPECT_EQ(cull[0].buffer, fake_buffer(0x10));
    EXPECT_EQ(cull[0].size, 256U);
    EXPECT_TRUE(cull[0].srcStageMask & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    EXPECT_TRUE(cull[0].srcAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT);
    EXPECT_TRUE(cull[0].dstStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    EXPECT_TRUE(cull[0].dstAccessMask & VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    EXPECT_TRUE(cull[0].dstAccessMask & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    const auto light = graph.planned_buffer_barriers("light");
    ASSERT_EQ(light.size(), 1U);
    EXPECT_TRUE(light[0].srcAccessMask & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    EXPECT_TRUE(light[0].dstStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    EXPECT_TRUE(light[0].dstAccessMask & VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    EXPECT_TRUE(graph.planned_buffer_barriers("light_again").empty());

    const auto forward = graph.planned_buffer_barriers("forward");
    ASSERT_EQ(forward.size(), 1U);
    EXPECT_TRUE(forward[0].srcStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    EXPECT_TRUE(forward[0].dstStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

    // Buffers never produce image barriers.
    EXPECT_TRUE(graph.planned_barriers("cull").empty());
}

// Write-after-read: a later write waits for every reader's stage, and the
// graph orders it after them even when declared first.
TEST(RenderGraphBuffers, WriteAfterReadWaitsForTheReaders) {
    RenderGraph graph;
    const auto buf = graph.import_buffer("b", fake_buffer(0x20), 64);
    (void)graph.add_pass("w1", PassType::compute).writes(buf, ResourceUsage::buffer_write_compute);
    (void)graph.add_pass("r_frag", PassType::graphics)
        .reads(buf, ResourceUsage::buffer_read_fragment);
    (void)graph.add_pass("r_comp", PassType::compute).reads(buf, ResourceUsage::buffer_read_compute);
    (void)graph.add_pass("w2", PassType::compute).writes(buf, ResourceUsage::buffer_write_compute);
    ASSERT_OK(graph.compile());
    const auto b = graph.planned_buffer_barriers("w2");
    ASSERT_EQ(b.size(), 1U);
    EXPECT_TRUE(b[0].srcStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    EXPECT_TRUE(b[0].srcStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

// A buffer-only hop plans buffer barriers (no image barriers), and binding()
// hands passes the buffer handle + size.
TEST(RenderGraphBuffers, BindingResolvesTheBuffer) {
    RenderGraph graph;
    const auto buf = graph.import_buffer("b", fake_buffer(0x30), 64);
    (void)graph.add_pass("producer", PassType::compute)
        .writes(buf, ResourceUsage::buffer_write_compute);
    (void)graph.add_pass("consumer", PassType::compute)
        .reads(buf, ResourceUsage::buffer_read_compute);
    ASSERT_OK(graph.compile());
    EXPECT_EQ(graph.planned_buffer_barriers("consumer").size(), 1U);
    EXPECT_TRUE(graph.planned_barriers("consumer").empty());
    EXPECT_EQ(graph.binding(buf).buffer, fake_buffer(0x30));
    EXPECT_EQ(graph.binding(buf).size, 64U);
    EXPECT_EQ(graph.binding(buf).image, VK_NULL_HANDLE);
}

// An image usage on a buffer (or a buffer usage on an image) is rejected.
TEST(RenderGraphBuffers, MismatchedUsageIsAnError) {
    RenderGraph graph;
    const auto buf = graph.import_buffer("b", fake_buffer(0x40), 64);
    (void)graph.add_pass("bad", PassType::compute).reads(buf, ResourceUsage::sampled_compute);
    ASSERT_ERR(graph.compile());
}

// The imported buffer's available stage is waited on by its first access.
TEST(RenderGraphBuffers, ImportedBufferFirstAccessWaitsOnItsAvailableStage) {
    RenderGraph graph;
    const auto buf = graph.import_buffer("b", fake_buffer(0x50), 64,
                                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                         VK_ACCESS_2_TRANSFER_WRITE_BIT);
    (void)graph.add_pass("read", PassType::compute).reads(buf, ResourceUsage::buffer_read_compute);
    ASSERT_OK(graph.compile());
    const auto b = graph.planned_buffer_barriers("read");
    ASSERT_EQ(b.size(), 1U);
    EXPECT_EQ(b[0].srcStageMask, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
}

} // namespace
