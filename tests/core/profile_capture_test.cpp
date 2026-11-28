// Frame captures (R4.3): the state machine (start, frames advance, GPU
// frames arriving late, result, unknown ids), input bounds, rankings and
// spikes, ranked by self time so a call chain does not crowd out other
// causes.

#include <gtest/gtest.h>

#include <vector>

#include "engine/core/profile_capture.hpp"

namespace {

using engine::core::CaptureRequest;
using engine::core::ProfileCaptures;
using engine::core::ProfileCategory;
using engine::core::ZoneSample;

struct Zones {
    engine::core::ProfileNameId cheap = engine::core::intern_profile_name(
        "capture_test.cheap", ProfileCategory::system);
    engine::core::ProfileNameId heavy = engine::core::intern_profile_name(
        "capture_test.heavy", ProfileCategory::pass);
    engine::core::ProfileNameId wait = engine::core::intern_profile_name(
        "capture_test.wait", ProfileCategory::wait);
};

TEST(ProfileCapture, RejectsOutOfRangeRequests) {
    ProfileCaptures c;
    EXPECT_FALSE(c.start({.frames = 0}));
    EXPECT_FALSE(c.start({.frames = engine::core::k_capture_max_frames + 1}));
    EXPECT_FALSE(c.start({.frames = 10, .budget_ms = 0.0F}));
    EXPECT_FALSE(c.start({.frames = 10, .budget_ms = 5000.0F}));
    EXPECT_FALSE(c.start({.frames = 10, .top = 0}));
    EXPECT_FALSE(c.start({.frames = 10, .top = engine::core::k_capture_max_top + 1}));
    const auto busy_check = c.start({.frames = 3});
    ASSERT_TRUE(busy_check);
    const auto busy = c.start({.frames = 3});
    ASSERT_FALSE(busy);
    EXPECT_NE(busy.error().message.find("busy"), std::string::npos);
}

TEST(ProfileCapture, RecordsFramesThenWaitsForTheirGpuTimings) {
    const Zones z;
    ProfileCaptures c;
    const auto id = c.start({.frames = 3, .budget_ms = 10.0F, .top = 5});
    ASSERT_TRUE(id);
    EXPECT_EQ(*id, 1U);
    EXPECT_TRUE(c.recording());
    EXPECT_EQ(c.result(*id), nullptr);

    for (std::uint64_t f = 100; f < 103; ++f) {
        const std::vector<ZoneSample> cpu{
            {z.cheap, 1.0F, 1.0F}, {z.heavy, 2.0F, 2.0F}, {z.wait, 3.0F, 3.0F}};
        c.on_cpu_frame(f, cpu, 8.0F, 5.0F, 3.0F);
        if (f >= 101) { // GPU timings lag one frame here
            const std::vector<ZoneSample> gpu{{z.heavy, 4.0F, 4.0F}};
            c.on_gpu_frame(f - 1, gpu, 6.0F);
        }
    }
    auto progress = c.progress(*id);
    ASSERT_TRUE(progress);
    EXPECT_EQ(progress->frames_done, 3U);
    EXPECT_TRUE(progress->waiting_for_gpu);

    const std::vector<ZoneSample> gpu{{z.heavy, 12.0F, 12.0F}}; // the last frame: a GPU spike
    c.on_gpu_frame(102, gpu, 14.0F);
    EXPECT_FALSE(c.recording());
    EXPECT_FALSE(c.progress(*id));
    const auto* s = c.result(*id);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->frames, 3U);
    EXPECT_EQ(s->first_frame, 100U);
    EXPECT_EQ(s->gpu_frames, 3U);
    EXPECT_FLOAT_EQ(s->total.avg, 8.0F);
    EXPECT_FLOAT_EQ(s->wait.avg, 3.0F);
    EXPECT_FLOAT_EQ(s->gpu.max, 14.0F);
    EXPECT_EQ(s->frame_total_ms, (std::vector<float>{8.0F, 8.0F, 8.0F}));
    EXPECT_EQ(s->frame_gpu_ms, (std::vector<float>{6.0F, 6.0F, 14.0F}));

    // Ranked by the larger of CPU self and GPU totals: heavy (GPU 20 ms) first.
    ASSERT_EQ(s->top_by_total.size(), 3U);
    EXPECT_EQ(s->top_by_total[0].name, "capture_test.heavy");
    EXPECT_DOUBLE_EQ(s->top_by_total[0].gpu_total_ms, 20.0);
    EXPECT_DOUBLE_EQ(s->top_by_total[0].cpu_total_ms, 6.0);
    EXPECT_EQ(s->top_by_total[0].frames, 3U);
    EXPECT_FLOAT_EQ(s->top_by_p95[0].gpu.p95, 12.0F);

    // Frame 102's GPU time (14 ms) is over the 10 ms budget; its worst zone
    // is the heavy pass on the GPU, and wait zones are never blamed.
    EXPECT_EQ(s->spike_count, 1U);
    ASSERT_EQ(s->spikes.size(), 1U);
    EXPECT_EQ(s->spikes[0].frame, 102U);
    ASSERT_FALSE(s->spikes[0].worst.empty());
    EXPECT_EQ(s->spikes[0].worst[0].name, "capture_test.heavy");
    EXPECT_TRUE(s->spikes[0].worst[0].gpu);
    for (const auto& w : s->spikes[0].worst) EXPECT_NE(w.name, "capture_test.wait");
}

TEST(ProfileCapture, SpikesAndRankingsUseSelfTime) {
    // One CPU spike: update_world (14 ms) > physics_system (12.1) >
    // physics_step (12) is one call chain, script_on_update (3) a second
    // cause. Inclusive ranking would fill all three spike slots with the
    // chain; self time names physics_step, script_on_update, then the
    // wrappers' own (small) work.
    const auto world = engine::core::intern_profile_name("self_test.update_world",
                                                         ProfileCategory::system);
    const auto physics = engine::core::intern_profile_name("self_test.physics_system",
                                                           ProfileCategory::system);
    const auto step = engine::core::intern_profile_name("self_test.physics_step");
    const auto script = engine::core::intern_profile_name("self_test.script_on_update");
    ProfileCaptures c;
    c.set_gpu_expected(false);
    const auto id = c.start({.frames = 1, .budget_ms = 10.0F, .top = 4});
    ASSERT_TRUE(id);
    const std::vector<ZoneSample> cpu{{step, 12.0F, 12.0F},
                                      {physics, 12.1F, 0.1F},
                                      {script, 3.0F, 3.0F},
                                      {world, 15.6F, 0.5F}};
    c.on_cpu_frame(0, cpu, 16.0F, 16.0F, 0.0F);
    const auto* s = c.result(*id);
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->spikes.size(), 1U);
    const auto& worst = s->spikes[0].worst;
    ASSERT_EQ(worst.size(), engine::core::k_capture_spike_zones);
    EXPECT_EQ(worst[0].name, "self_test.physics_step");
    EXPECT_EQ(worst[1].name, "self_test.script_on_update");
    EXPECT_EQ(worst[2].name, "self_test.update_world");
    EXPECT_FLOAT_EQ(worst[2].ms, 15.6F) << "the inclusive time is still reported";
    EXPECT_FLOAT_EQ(worst[2].self_ms, 0.5F);

    ASSERT_EQ(s->top_by_total.size(), 4U);
    EXPECT_EQ(s->top_by_total[0].name, "self_test.physics_step");
    EXPECT_EQ(s->top_by_total[1].name, "self_test.script_on_update");
    EXPECT_DOUBLE_EQ(s->top_by_total.back().cpu_self_total_ms, 0.1F);
    EXPECT_DOUBLE_EQ(s->top_by_total.back().cpu_total_ms, 12.1F);
    EXPECT_EQ(s->top_by_p95[0].name, "self_test.physics_step");
}

TEST(ProfileCapture, FinishesWithoutGpuWhenTimingIsUnavailable) {
    ProfileCaptures c;
    c.set_gpu_expected(false);
    const auto id = c.start({.frames = 2});
    ASSERT_TRUE(id);
    c.on_cpu_frame(5, {}, 20.0F, 20.0F, 0.0F);
    EXPECT_TRUE(c.recording());
    c.on_cpu_frame(6, {}, 10.0F, 10.0F, 0.0F);
    const auto* s = c.result(*id);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->gpu_frames, 0U);
    EXPECT_EQ(s->gpu.count, 0U);
    EXPECT_EQ(s->frame_gpu_ms, (std::vector<float>{-1.0F, -1.0F}));
    EXPECT_EQ(s->spike_count, 1U); // 20 ms > 16.667 ms default budget
    EXPECT_EQ(s->spikes[0].frame, 5U);
}

TEST(ProfileCapture, GivesUpOnMissingGpuFramesAfterAGrace) {
    ProfileCaptures c;
    const auto id = c.start({.frames = 1});
    ASSERT_TRUE(id);
    c.on_cpu_frame(0, {}, 5.0F, 5.0F, 0.0F);
    for (std::uint32_t i = 1; i < engine::core::k_capture_gpu_grace_frames; ++i) {
        c.on_cpu_frame(i, {}, 5.0F, 5.0F, 0.0F);
        EXPECT_TRUE(c.recording()) << i;
    }
    c.on_cpu_frame(engine::core::k_capture_gpu_grace_frames, {}, 5.0F, 5.0F, 0.0F);
    ASSERT_NE(c.result(*id), nullptr);
    EXPECT_EQ(c.result(*id)->gpu_frames, 0U);
}

TEST(ProfileCapture, IgnoresGpuFramesOutsideTheCapture) {
    ProfileCaptures c;
    const auto id = c.start({.frames = 1});
    ASSERT_TRUE(id);
    c.on_gpu_frame(3, {}, 1.0F); // before the capture started: ignored
    c.on_cpu_frame(10, {}, 5.0F, 5.0F, 0.0F);
    c.on_gpu_frame(9, {}, 1.0F);  // before its first frame
    c.on_gpu_frame(11, {}, 1.0F); // after its last
    EXPECT_TRUE(c.recording());
    c.on_gpu_frame(10, {}, 2.0F);
    ASSERT_NE(c.result(*id), nullptr);
    EXPECT_FLOAT_EQ(c.result(*id)->gpu.avg, 2.0F);
}

TEST(ProfileCapture, UnknownIdsAndKeptResults) {
    ProfileCaptures c;
    c.set_gpu_expected(false);
    EXPECT_EQ(c.result(1), nullptr);
    EXPECT_FALSE(c.progress(1));
    EXPECT_EQ(c.last_id(), 0U);
    std::uint64_t frame = 0;
    for (std::size_t i = 0; i < engine::core::k_capture_kept_results + 2; ++i) {
        ASSERT_TRUE(c.start({.frames = 1}));
        c.on_cpu_frame(frame++, {}, 1.0F, 1.0F, 0.0F);
    }
    const auto last = c.last_id();
    EXPECT_EQ(last, engine::core::k_capture_kept_results + 2);
    EXPECT_EQ(c.result(1), nullptr); // dropped: only the newest are kept
    EXPECT_EQ(c.result(2), nullptr);
    EXPECT_NE(c.result(3), nullptr);
    EXPECT_NE(c.result(last), nullptr);
    EXPECT_EQ(c.result(last + 1), nullptr);

    c.clear();
    EXPECT_EQ(c.result(last), nullptr);
    const auto next = c.start({.frames = 1});
    ASSERT_TRUE(next);
    EXPECT_EQ(*next, last + 1); // ids keep counting after clear()
}

} // namespace
