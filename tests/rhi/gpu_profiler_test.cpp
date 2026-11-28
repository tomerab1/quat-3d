// GPU pass timing (R4.2): two render-graph passes timed through
// GpuProfiler resolve to non-negative, ordered durations inside the frame,
// each frame is collected once, zones ended out of order or never ended
// are still closed inside their frame, and a profiler without a device does
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

TEST(GpuProfilerTiming, OutOfOrderAndUnendedZonesCloseInTheirFrame) {
    QUAT_REQUIRE_GPU(ctx);
    auto profiler = GpuProfiler::create(
        ctx->device, GpuProfiler::Config{.frames_in_flight = 2, .max_zones = 8, .tracy = false});
    ASSERT_OK(profiler);
    if (!profiler->supported()) GTEST_SKIP() << profiler->status();
    const auto outer = engine::core::intern_profile_name("unwind_outer");
    const auto inner = engine::core::intern_profile_name("unwind_inner");
    const auto unended = engine::core::intern_profile_name("unwind_unended");

    GpuProfiler::ZoneToken next_frame_token = GpuProfiler::k_no_zone;
    ASSERT_OK(quat::test::submit_and_wait(ctx->device, [&](VkCommandBuffer cmd) {
        profiler->begin_frame(cmd, 0, 7);
        const auto a = profiler->begin_zone(cmd, outer, "unwind_outer");
        const auto b = profiler->begin_zone(cmd, inner, "unwind_inner");
        EXPECT_EQ(a, 0);
        EXPECT_EQ(b, 1);
        profiler->end_zone(cmd, a); // outer first: closes inner too
        profiler->end_zone(cmd, b); // already closed: no-op
        (void)profiler->begin_zone(cmd, unended, "unwind_unended"); // never ended
        profiler->end_frame(cmd);   // closes it while cmd still records
        profiler->begin_frame(cmd, 1, 8);
        next_frame_token = profiler->begin_zone(cmd, outer, "unwind_outer");
        profiler->end_zone(cmd, next_frame_token);
        profiler->end_frame(cmd);
    }));
    EXPECT_EQ(next_frame_token, 0) << "depth starts at 0 in the next frame";

    const auto frame = profiler->collect(0);
    ASSERT_TRUE(frame) << "every query of the frame was written";
    ASSERT_EQ(frame->zones.size(), 3U);
    EXPECT_EQ(frame->zones[0].id, outer);
    EXPECT_EQ(frame->zones[1].id, inner);
    EXPECT_EQ(frame->zones[2].id, unended);
    EXPECT_EQ(frame->zones[1].depth, 1);
    EXPECT_EQ(frame->zones[2].depth, 0);
    constexpr float k_eps = 1e-4F;
    // The inner zone ends no later than the outer one (closed innermost first).
    EXPECT_LE(frame->zones[1].start_ms + frame->zones[1].ms,
              frame->zones[0].start_ms + frame->zones[0].ms + k_eps);
    EXPECT_TRUE(profiler->collect(1));
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
