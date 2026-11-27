// Transient image pool: reuse within a frame shape, eviction of idle images.

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

using engine::rhi::TransientImagePool;
constexpr VkImageUsageFlags k_usage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

TEST(GpuTransientPool, ReusesMatchingImagesAcrossFrames) {
    QUAT_REQUIRE_GPU(g);
    TransientImagePool pool(g->device, g->allocator);
    for (int frame = 0; frame < 10; ++frame) {
        ASSERT_OK(pool.acquire(VK_FORMAT_R8G8B8A8_UNORM, {64, 32}, k_usage));
        ASSERT_OK(pool.acquire(VK_FORMAT_R8G8B8A8_UNORM, {64, 32}, k_usage));
        pool.reset();
    }
    EXPECT_EQ(pool.size(), 2U); // two concurrent images, reused every frame
}

// Audit A5: every resize used to leave a full set of images at the old extent
// allocated for the life of the program.
TEST(GpuTransientPool, EvictsImagesIdleLongerThanTheLimit) {
    QUAT_REQUIRE_GPU(g);
    TransientImagePool pool(g->device, g->allocator);
    for (std::uint32_t w = 16; w < 16 + 20; ++w) { // a "resize" every frame
        ASSERT_OK(pool.acquire(VK_FORMAT_R8G8B8A8_UNORM, {w, 16}, k_usage));
        pool.reset();
    }
    EXPECT_LE(pool.size(), TransientImagePool::max_idle_resets + 1);

    for (std::uint32_t i = 0; i <= TransientImagePool::max_idle_resets; ++i) pool.reset();
    EXPECT_EQ(pool.size(), 0U) << "idle images were not released";
}

} // namespace
