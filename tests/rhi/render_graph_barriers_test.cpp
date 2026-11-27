// Render-graph barrier planning (audit A3/A4/A7, issue #4).

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

// R10.2: sampled_graphics (vertex texture fetch) transitions to
// SHADER_READ_ONLY and makes the prior write visible to the vertex stage too.
TEST(GpuRenderGraph, SampledGraphicsReadCoversTheVertexStage) {
    QUAT_REQUIRE_GPU(g);
    TransientImagePool pool(g->device, g->allocator);
    RenderGraph graph(pool);
    const auto img = graph.create_transient_image(
        "heights", VK_FORMAT_R32_SFLOAT, k_extent,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    (void)graph.add_pass("upload", PassType::transfer).writes(img, ResourceUsage::transfer_dst);
    (void)graph.add_pass("draw", PassType::graphics).reads(img, ResourceUsage::sampled_graphics);
    ASSERT_OK(graph.compile());
    const auto b = graph.planned_barriers("draw");
    ASSERT_EQ(b.size(), 1U);
    EXPECT_EQ(b[0].newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_TRUE(b[0].srcAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT);
    EXPECT_TRUE(b[0].dstStageMask & VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    EXPECT_TRUE(b[0].dstStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    EXPECT_TRUE(b[0].dstAccessMask & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
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
// Layout transitions done by reads (issue #4). A transition is a write in the
// synchronization model: it runs after its barrier's source scope and before
// its destination scope. A later reader in the new layout, outside that
// destination scope, must chain onto it (its barrier's source stages include
// the transitioning barrier's destination stages); waiting on the original
// writer alone does not order it after the transition. Imported images with
// fake handles need no device: barrier planning is pure bookkeeping.
// ---------------------------------------------------------------------------

[[nodiscard]] VkImage fake_image(std::uintptr_t id) { return std::bit_cast<VkImage>(id); }

[[nodiscard]] engine::rhi::ResourceHandle
import_fake_image(RenderGraph& graph, std::uintptr_t id, VkFormat format,
                  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED) {
    return graph.import_image("img", fake_image(id), VK_NULL_HANDLE, format, k_extent, layout,
                              VK_IMAGE_LAYOUT_UNDEFINED);
}

// The reported sequence: depth written as an attachment, sampled by compute
// (DEPTH_ATTACHMENT -> SHADER_READ_ONLY, dst COMPUTE), then sampled by a
// fragment shader in the same layout. The fragment barrier must wait on the
// compute stage that the transition happened before, as well as the write.
TEST(RenderGraphTransitions, SameLayoutReadAfterTransitioningReadWaitsOnTheTransition) {
    RenderGraph graph;
    const auto depth = import_fake_image(graph, 0x100, VK_FORMAT_D32_SFLOAT);
    (void)graph.add_pass("write", PassType::graphics).writes(depth, ResourceUsage::depth_attachment);
    (void)graph.add_pass("read_compute", PassType::compute)
        .reads(depth, ResourceUsage::sampled_compute);
    (void)graph.add_pass("read_fragment", PassType::graphics).reads(depth, ResourceUsage::sampled);
    (void)graph.add_pass("read_compute_again", PassType::compute)
        .reads(depth, ResourceUsage::sampled_compute);
    ASSERT_OK(graph.compile());

    const auto transition = graph.planned_barriers("read_compute");
    ASSERT_EQ(transition.size(), 1U);
    EXPECT_EQ(transition[0].oldLayout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(transition[0].newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(transition[0].dstStageMask, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    const auto frag = graph.planned_barriers("read_fragment");
    ASSERT_EQ(frag.size(), 1U);
    EXPECT_EQ(frag[0].oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(frag[0].newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_TRUE(frag[0].srcStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
        << "fragment read is not ordered after the layout transition";
    EXPECT_TRUE(frag[0].srcStageMask & VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
    EXPECT_TRUE(frag[0].srcAccessMask & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    EXPECT_EQ(frag[0].dstStageMask, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    EXPECT_EQ(frag[0].dstAccessMask, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // Minimal: a reader the transition barrier already covered needs nothing.
    EXPECT_TRUE(graph.planned_barriers("read_compute_again").empty());
}

// Several later readers in other stages: each gets one barrier chained onto
// the transition; once a stage has been covered it needs no further barrier.
TEST(RenderGraphTransitions, EveryNewStageAfterATransitionChainsOntoIt) {
    RenderGraph graph;
    const auto img = import_fake_image(graph, 0x110, VK_FORMAT_R16G16B16A16_SFLOAT);
    (void)graph.add_pass("write", PassType::compute).writes(img, ResourceUsage::storage_write);
    (void)graph.add_pass("read_fragment", PassType::graphics).reads(img, ResourceUsage::sampled);
    (void)graph.add_pass("read_vertex_fragment", PassType::graphics)
        .reads(img, ResourceUsage::sampled_graphics);
    (void)graph.add_pass("read_compute", PassType::compute)
        .reads(img, ResourceUsage::sampled_compute);
    (void)graph.add_pass("read_fragment_again", PassType::graphics)
        .reads(img, ResourceUsage::sampled);
    ASSERT_OK(graph.compile());

    const auto transition = graph.planned_barriers("read_fragment");
    ASSERT_EQ(transition.size(), 1U);
    EXPECT_EQ(transition[0].oldLayout, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(transition[0].dstStageMask, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

    const auto vf = graph.planned_barriers("read_vertex_fragment");
    ASSERT_EQ(vf.size(), 1U);
    EXPECT_TRUE(vf[0].srcStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT); // transition
    EXPECT_TRUE(vf[0].srcStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);  // write
    EXPECT_TRUE(vf[0].dstStageMask & VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);

    const auto comp = graph.planned_barriers("read_compute");
    ASSERT_EQ(comp.size(), 1U);
    EXPECT_TRUE(comp[0].srcStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    EXPECT_EQ(comp[0].dstStageMask, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    EXPECT_EQ(comp[0].oldLayout, comp[0].newLayout);

    EXPECT_TRUE(graph.planned_barriers("read_fragment_again").empty());
}

// A transfer write, then a graphics read that transitions (dst VERTEX |
// FRAGMENT), then a compute read: the compute read chains onto both stages
// of the transitioning barrier's destination scope.
TEST(RenderGraphTransitions, ComputeReadAfterGraphicsTransitionChainsOntoIt) {
    RenderGraph graph;
    const auto img = import_fake_image(graph, 0x120, VK_FORMAT_R32_SFLOAT);
    (void)graph.add_pass("upload", PassType::transfer).writes(img, ResourceUsage::transfer_dst);
    (void)graph.add_pass("draw", PassType::graphics).reads(img, ResourceUsage::sampled_graphics);
    (void)graph.add_pass("cull", PassType::compute).reads(img, ResourceUsage::sampled_compute);
    ASSERT_OK(graph.compile());
    const auto b = graph.planned_barriers("cull");
    ASSERT_EQ(b.size(), 1U);
    EXPECT_TRUE(b[0].srcStageMask & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    EXPECT_TRUE(b[0].srcStageMask & VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    EXPECT_TRUE(b[0].srcStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    EXPECT_TRUE(b[0].srcAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT);
    EXPECT_EQ(b[0].dstStageMask, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

// An imported image with nothing to wait for (idle, already in GENERAL) is
// transitioned by its first reader. That transition is still a write: a
// reader in another stage must wait on it even though no pass wrote the image.
TEST(RenderGraphTransitions, TransitionWithoutAPriorWriteStillOrdersLaterReaders) {
    RenderGraph graph;
    const auto img =
        import_fake_image(graph, 0x130, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL);
    (void)graph.add_pass("read_compute", PassType::compute)
        .reads(img, ResourceUsage::sampled_compute);
    (void)graph.add_pass("read_fragment", PassType::graphics).reads(img, ResourceUsage::sampled);
    ASSERT_OK(graph.compile());
    const auto transition = graph.planned_barriers("read_compute");
    ASSERT_EQ(transition.size(), 1U);
    EXPECT_EQ(transition[0].srcStageMask, VK_PIPELINE_STAGE_2_NONE);
    const auto frag = graph.planned_barriers("read_fragment");
    ASSERT_EQ(frag.size(), 1U);
    EXPECT_EQ(frag[0].srcStageMask, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    EXPECT_EQ(frag[0].dstStageMask, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
}

// Readers in the old layout complete before the transition (its source scope
// holds them), and a write after the new-layout readers waits for them and
// for the transition.
TEST(RenderGraphTransitions, OldLayoutReadersCompleteBeforeTheTransition) {
    RenderGraph graph;
    const auto img = import_fake_image(graph, 0x140, VK_FORMAT_R16G16B16A16_SFLOAT);
    (void)graph.add_pass("write", PassType::compute).writes(img, ResourceUsage::storage_write);
    (void)graph.add_pass("read_storage", PassType::compute)
        .reads(img, ResourceUsage::storage_read);
    (void)graph.add_pass("read_fragment", PassType::graphics).reads(img, ResourceUsage::sampled);
    (void)graph.add_pass("rewrite", PassType::compute).writes(img, ResourceUsage::storage_write);
    ASSERT_OK(graph.compile());

    const auto transition = graph.planned_barriers("read_fragment");
    ASSERT_EQ(transition.size(), 1U);
    EXPECT_EQ(transition[0].oldLayout, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT_EQ(transition[0].newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_TRUE(transition[0].srcStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    const auto rewrite = graph.planned_barriers("rewrite");
    ASSERT_EQ(rewrite.size(), 1U);
    EXPECT_EQ(rewrite[0].oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_TRUE(rewrite[0].srcStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
}

// A write supersedes an earlier transition: a read after it chains onto the
// write and onto any transition made since, not onto the stale one.
TEST(RenderGraphTransitions, WriteAfterTransitionResetsTheChain) {
    RenderGraph graph;
    const auto img = import_fake_image(graph, 0x150, VK_FORMAT_R16G16B16A16_SFLOAT);
    (void)graph.add_pass("write", PassType::compute).writes(img, ResourceUsage::storage_write);
    (void)graph.add_pass("read_fragment", PassType::graphics).reads(img, ResourceUsage::sampled);
    (void)graph.add_pass("upload", PassType::transfer).writes(img, ResourceUsage::transfer_dst);
    (void)graph.add_pass("read_compute", PassType::compute)
        .reads(img, ResourceUsage::sampled_compute);
    (void)graph.add_pass("read_fragment_2", PassType::graphics).reads(img, ResourceUsage::sampled);
    ASSERT_OK(graph.compile());
    const auto frag = graph.planned_barriers("read_fragment_2");
    ASSERT_EQ(frag.size(), 1U);
    EXPECT_EQ(frag[0].srcStageMask,
              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    EXPECT_EQ(frag[0].srcAccessMask, VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

// ---------------------------------------------------------------------------
// Final-layout transitions of imported images (issue #9). The trailing
// barrier's second scope is the stage the next consumer waits at, never NONE:
// a transition ordered before nothing is not covered by a later semaphore
// signal, so present could race it.
// ---------------------------------------------------------------------------

// Default: ALL_COMMANDS, which any consumer's wait chains onto.
TEST(RenderGraphFinalLayout, FinalBarrierHasARealSecondScope) {
    RenderGraph graph;
    const auto img = graph.import_image("viewport", fake_image(0x200), VK_NULL_HANDLE,
                                        VK_FORMAT_R8G8B8A8_UNORM, k_extent,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    (void)graph.add_pass("draw", PassType::graphics).writes(img, ResourceUsage::color_attachment);
    ASSERT_OK(graph.compile());
    const auto& b = graph.planned_final_barriers();
    ASSERT_EQ(b.size(), 1U);
    EXPECT_EQ(b[0].oldLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(b[0].newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_NE(b[0].dstStageMask, VK_PIPELINE_STAGE_2_NONE);
    EXPECT_EQ(b[0].dstStageMask, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    EXPECT_EQ(b[0].dstAccessMask, 0U);
    EXPECT_TRUE(b[0].srcStageMask & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
}

// A swapchain image: the final PRESENT_SRC transition's second scope is the
// stage the render-finished semaphore is signalled at.
TEST(RenderGraphFinalLayout, PresentTransitionEndsAtTheSignalStage) {
    RenderGraph graph;
    constexpr VkPipelineStageFlags2 k_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const auto img = graph.import_image("swapchain", fake_image(0x210), VK_NULL_HANDLE,
                                        VK_FORMAT_B8G8R8A8_SRGB, k_extent,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, k_stage, 0, k_stage);
    (void)graph.add_pass("resolve", PassType::transfer).writes(img, ResourceUsage::transfer_dst);
    ASSERT_OK(graph.compile());
    const auto& b = graph.planned_final_barriers();
    ASSERT_EQ(b.size(), 1U);
    EXPECT_EQ(b[0].newLayout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    EXPECT_EQ(b[0].dstStageMask, k_stage);
    EXPECT_TRUE(b[0].srcStageMask & VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    EXPECT_TRUE(b[0].srcAccessMask & VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

// An image left in its final layout by the last pass needs no final barrier.
TEST(RenderGraphFinalLayout, NoFinalBarrierWhenAlreadyInTheFinalLayout) {
    RenderGraph graph;
    const auto img = graph.import_image("img", fake_image(0x220), VK_NULL_HANDLE,
                                        VK_FORMAT_R8G8B8A8_UNORM, k_extent,
                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    (void)graph.add_pass("w", PassType::compute).writes(img, ResourceUsage::storage_write);
    ASSERT_OK(graph.compile());
    EXPECT_TRUE(graph.planned_final_barriers().empty());
}

// A NONE final stage with a final layout is a declaration bug.
TEST(RenderGraphFinalLayout, NoneFinalStageIsAnError) {
    RenderGraph graph;
    const auto img = graph.import_image("swapchain", fake_image(0x230), VK_NULL_HANDLE,
                                        VK_FORMAT_B8G8R8A8_SRGB, k_extent,
                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_NONE);
    (void)graph.add_pass("w", PassType::transfer).writes(img, ResourceUsage::transfer_dst);
    ASSERT_ERR(graph.compile());
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

// Compute-generated draws (R9.3): the indirect-argument read waits on the
// compute write at DRAW_INDIRECT, the instance read at the vertex shader.
TEST(RenderGraphBuffers, IndirectAndVertexReadsWaitOnTheComputeWrite) {
    RenderGraph graph;
    const auto args = graph.import_buffer("args", fake_buffer(0x60), 64);
    const auto items = graph.import_buffer("items", fake_buffer(0x70), 1024);
    (void)graph.add_pass("generate", PassType::compute)
        .writes(args, ResourceUsage::buffer_write_compute)
        .writes(items, ResourceUsage::buffer_write_compute);
    (void)graph.add_pass("draw", PassType::graphics)
        .reads(args, ResourceUsage::buffer_read_indirect)
        .reads(items, ResourceUsage::buffer_read_vertex);
    ASSERT_OK(graph.compile());
    const auto b = graph.planned_buffer_barriers("draw");
    ASSERT_EQ(b.size(), 2U);
    for (const auto& barrier : b) {
        EXPECT_TRUE(barrier.srcStageMask & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        EXPECT_TRUE(barrier.srcAccessMask & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        if (barrier.buffer == fake_buffer(0x60)) {
            EXPECT_EQ(barrier.dstStageMask, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
            EXPECT_EQ(barrier.dstAccessMask, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        } else {
            EXPECT_EQ(barrier.buffer, fake_buffer(0x70));
            EXPECT_TRUE(barrier.dstStageMask & VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
            EXPECT_TRUE(barrier.dstAccessMask & VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        }
    }
}

// Indirect / vertex reads are buffer-only usages.
TEST(RenderGraphBuffers, IndirectUsageOnAnImageIsAnError) {
    RenderGraph graph;
    const auto img = graph.import_image("img", VK_NULL_HANDLE, VK_NULL_HANDLE,
                                        VK_FORMAT_R8G8B8A8_UNORM, k_extent,
                                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    (void)graph.add_pass("bad", PassType::graphics).reads(img, ResourceUsage::buffer_read_indirect);
    ASSERT_ERR(graph.compile());
}

// R10.3: a read-only depth pass (depth-tested and sampled in the same pass,
// soft particles) moves the depth to DEPTH_READ_ONLY_OPTIMAL and makes the
// depth writes visible to the fragment tests and the fragment shader; a
// later sampled read transitions it again.
TEST(RenderGraphDepth, DepthReadTransitionsToReadOnlyForTestsAndSampling) {
    RenderGraph graph;
    const auto depth = graph.import_image("depth", VK_NULL_HANDLE, VK_NULL_HANDLE,
                                          VK_FORMAT_D32_SFLOAT, k_extent,
                                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED);
    (void)graph.add_pass("opaque", PassType::graphics).writes(depth, ResourceUsage::depth_attachment);
    (void)graph.add_pass("particles", PassType::graphics).reads(depth, ResourceUsage::depth_read);
    (void)graph.add_pass("taa", PassType::graphics).reads(depth, ResourceUsage::sampled);
    ASSERT_OK(graph.compile());
    const auto b = graph.planned_barriers("particles");
    ASSERT_EQ(b.size(), 1U);
    EXPECT_EQ(b[0].oldLayout, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(b[0].newLayout, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    EXPECT_EQ(b[0].subresourceRange.aspectMask, VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT});
    EXPECT_TRUE(b[0].srcAccessMask & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    EXPECT_TRUE(b[0].dstStageMask & VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT);
    EXPECT_TRUE(b[0].dstStageMask & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    EXPECT_TRUE(b[0].dstAccessMask & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    EXPECT_TRUE(b[0].dstAccessMask & VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const auto t = graph.planned_barriers("taa");
    ASSERT_EQ(t.size(), 1U);
    EXPECT_EQ(t[0].newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace
