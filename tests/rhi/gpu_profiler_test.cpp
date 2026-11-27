// GPU pass timing (R4.2): two render-graph passes timed through
// GpuProfiler resolve to non-negative, ordered durations inside the frame,
// each frame is collected once, and a profiler without a device does
// nothing.

#include <gtest/gtest.h>

#include "engine/core/profile_names.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/gpu_profiler.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace {

using engine::rhi::GpuProfiler;
using engine::rhi::PassContext;
using engine::rhi::PassType;
using engine::rhi::RenderGraph;
using engine::rhi::ResourceUsage;

TEST(GpuProfilerTiming, TwoPassesResolveToOrderedDurations) {
    QUAT_REQUIRE_GPU(ctx);
    auto profiler = GpuProfiler::create(
        ctx->device, GpuProfiler::Config{.frames_in_flight = 2, .max_zones = 8, .tracy = false});
    ASSERT_OK(profiler);
    if (!profiler->supported()) GTEST_SKIP() << profiler->status();
    EXPECT_GT(profiler->valid_bits(), 0U);
    EXPECT_GT(profiler->period_ns(), 0.0F);

    engine::rhi::TransientImagePool pool(ctx->device, ctx->allocator);
    RenderGraph graph(pool);
    const auto image = graph.create_transient_image(
        "profiled_target", VK_FORMAT_R8G8B8A8_UNORM, VkExtent2D{256, 256},
        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const auto clear = [image](float value) {
        return [image, value](PassContext& pc) {
            const VkClearColorValue colour{{value, value, value, 1.0F}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(pc.cmd(), pc.resolve(image).image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
        };
    };
    graph.add_pass("profiled_first", PassType::transfer)
        .writes(image, ResourceUsage::transfer_dst)
        .execute(clear(0.25F));
    graph.add_pass("profiled_second", PassType::transfer)
        .writes(image, ResourceUsage::transfer_dst)
        .execute(clear(0.75F));
    ASSERT_OK(graph.compile());

    EXPECT_FALSE(profiler->collect(1)); // nothing timed in this slot yet
    ASSERT_OK(quat::test::submit_and_wait(ctx->device, [&](VkCommandBuffer cmd) {
        profiler->begin_frame(cmd, 1, 41);
        graph.execute(cmd, &*profiler);
        profiler->end_frame(cmd);
    }));

    const auto frame = profiler->collect(1);
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->frame_index, 41U);
    ASSERT_EQ(frame->zones.size(), 2U);
    const auto first = engine::core::intern_profile_name("profiled_first");
    const auto second = engine::core::intern_profile_name("profiled_second");
    EXPECT_EQ(frame->zones[0].id, first);
    EXPECT_EQ(frame->zones[1].id, second);
    constexpr float k_eps = 1e-4F; // tick rounding
    for (const auto& z : frame->zones) {
        EXPECT_GE(z.start_ms, 0.0F);
        EXPECT_GE(z.ms, 0.0F);
        EXPECT_EQ(z.depth, 0);
    }
    // Sequential, non-overlapping, inside the frame.
    EXPECT_GE(frame->zones[1].start_ms + k_eps, frame->zones[0].start_ms + frame->zones[0].ms);
    EXPECT_GE(frame->total_ms + k_eps, frame->zones[1].start_ms + frame->zones[1].ms);
    EXPECT_LT(frame->total_ms, 10'000.0F); // sane: not a wrapped counter

    EXPECT_FALSE(profiler->collect(1)); // a frame is returned once
}

TEST(NoDeviceGpuProfiler, ADefaultProfilerDoesNothing) {
    GpuProfiler profiler;
    EXPECT_FALSE(profiler.supported());
    EXPECT_FALSE(profiler.tracy_active());
    EXPECT_FALSE(profiler.collect(0));
    profiler.begin_frame(VK_NULL_HANDLE, 0, 0);
    const auto token = profiler.begin_zone(VK_NULL_HANDLE, 0, "nothing");
    EXPECT_EQ(token, GpuProfiler::k_no_zone);
    profiler.end_zone(VK_NULL_HANDLE, token);
    profiler.end_frame(VK_NULL_HANDLE);
    profiler.destroy();
}

} // namespace
