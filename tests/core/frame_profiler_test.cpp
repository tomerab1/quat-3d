// The built-in frame profiler (R4.2): scope recording on the bound thread,
// per-frame aggregation, wait / CPU split, GPU frames, counters, the window,
// sorting, name interning and the profile.hpp macros.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "engine/core/frame_profiler.hpp"
#include "engine/core/profile.hpp"

namespace {

using engine::core::FrameProfiler;
using engine::core::intern_profile_name;
using engine::core::ProfileCategory;
using engine::core::ProfileScope;
using engine::core::ScopeRecord;
using engine::core::ZoneSort;
using engine::core::ZoneSummary;

// Busy-waits so a scope has a measurable, non-zero duration.
void spin_for_us(int us) {
    const std::uint64_t until = engine::core::profile_now_ns() + static_cast<std::uint64_t>(us) * 1000;
    while (engine::core::profile_now_ns() < until) {
    }
}

[[nodiscard]] const ZoneSummary* find(const std::vector<ZoneSummary>& zones, std::string_view n) {
    const auto it = std::ranges::find_if(zones, [n](const ZoneSummary& z) { return z.name == n; });
    return it == zones.end() ? nullptr : &*it;
}

class FrameProfilerTest : public ::testing::Test {
protected:
    FrameProfiler profiler{FrameProfiler::Config{.window = 8, .scope_capacity = 64}};
    void SetUp() override { profiler.bind_current_thread(); }
    void TearDown() override { profiler.unbind_current_thread(); }
};

TEST(ProfileNames, InterningIsStableAndKeepsTheFirstCategory) {
    const auto a = intern_profile_name("profile_test.zone_a", ProfileCategory::pass);
    const auto again = intern_profile_name("profile_test.zone_a", ProfileCategory::system);
    EXPECT_EQ(a, again);
    EXPECT_EQ(engine::core::profile_name(a), "profile_test.zone_a");
    EXPECT_EQ(engine::core::profile_category(a), ProfileCategory::pass);
    EXPECT_NE(a, engine::core::k_profile_overflow_name);
    EXPECT_EQ(engine::core::profile_name(0xFFF0), "(other)"); // unknown id

    const std::string long_name(300, 'x');
    const auto truncated = intern_profile_name(long_name);
    EXPECT_EQ(engine::core::profile_name(truncated).size(), engine::core::k_max_profile_name_length);
}

TEST(ProfileNames, ThreadNamesGetIndices) {
    std::uint8_t worker = engine::core::k_unnamed_profile_thread;
    std::uint8_t before = 0;
    std::thread t([&] {
        before = engine::core::profile_thread_index();
        worker = engine::core::set_profile_thread_name("profile test worker");
    });
    t.join();
    EXPECT_EQ(before, engine::core::k_unnamed_profile_thread);
    ASSERT_NE(worker, engine::core::k_unnamed_profile_thread);
    EXPECT_EQ(engine::core::profile_thread_names().at(worker), "profile test worker");
}

TEST_F(FrameProfilerTest, ScopesAreSummedPerZoneAndFrame) {
    const auto work = intern_profile_name("profile_test.work", ProfileCategory::system);
    const auto pass = intern_profile_name("profile_test.pass", ProfileCategory::pass);
    {
        const ProfileScope outer(work);
        spin_for_us(200);
        const ProfileScope inner(pass);
        spin_for_us(100);
    }
    {
        const ProfileScope again(work);
        spin_for_us(100);
    }
    profiler.end_frame();

    const auto scopes = profiler.last_frame_scopes();
    ASSERT_EQ(scopes.size(), 3U);
    // Destruction order: the inner scope closes first, at depth 1.
    EXPECT_EQ(scopes[0].name, pass);
    EXPECT_EQ(scopes[0].depth, 1);
    EXPECT_EQ(scopes[1].name, work);
    EXPECT_EQ(scopes[1].depth, 0);
    for (const ScopeRecord& r : scopes) EXPECT_GE(r.end_ns, r.start_ns);

    const auto zones = profiler.zones();
    const ZoneSummary* w = find(zones, "profile_test.work");
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w->category, ProfileCategory::system);
    EXPECT_EQ(w->calls, 2U);
    EXPECT_EQ(w->cpu.count, 1U); // one frame
    EXPECT_GE(w->cpu.last, 0.4F); // 300 + 100 us at least
    EXPECT_EQ(w->gpu.count, 0U);
    const ZoneSummary* p = find(zones, "profile_test.pass");
    ASSERT_NE(p, nullptr);
    EXPECT_LT(p->cpu.last, w->cpu.last);
}

TEST_F(FrameProfilerTest, FrameTimesSplitIntoWaitAndCpu) {
    const auto wait = intern_profile_name("profile_test.wait", ProfileCategory::wait);
    profiler.end_frame(); // first frame: no start time, no frame sample
    EXPECT_EQ(profiler.frames_in_window(), 0U);
    {
        const ProfileScope w(wait);
        spin_for_us(500);
    }
    spin_for_us(300);
    profiler.end_frame();
    const auto f = profiler.frame_summary();
    ASSERT_EQ(f.total.count, 1U);
    EXPECT_GE(f.wait.last, 0.5F);
    EXPECT_GE(f.total.last, f.wait.last + 0.3F);
    EXPECT_NEAR(f.cpu.last, f.total.last - f.wait.last, 1e-4F);
    EXPECT_EQ(f.gpu.count, 0U);
    EXPECT_EQ(profiler.frame_index(), 2U);
}

TEST_F(FrameProfilerTest, GpuFramesJoinTheirZones) {
    const auto pass = intern_profile_name("profile_test.gpu_pass", ProfileCategory::pass);
    const std::vector<engine::core::GpuZoneTiming> timings{{pass, 1.5F}, {pass, 0.5F}};
    profiler.submit_gpu_frame(0, timings, 2.5F);
    const auto zones = profiler.zones();
    const ZoneSummary* z = find(zones, "profile_test.gpu_pass");
    ASSERT_NE(z, nullptr);
    EXPECT_EQ(z->gpu.count, 1U);
    EXPECT_FLOAT_EQ(z->gpu.last, 2.0F); // both calls summed
    EXPECT_EQ(z->cpu.count, 0U);
    EXPECT_FLOAT_EQ(profiler.frame_summary().gpu.last, 2.5F);
}

TEST_F(FrameProfilerTest, ZonesLeaveTheWindowWhenTheyStopRunning) {
    const auto once = intern_profile_name("profile_test.once");
    {
        const ProfileScope s(once);
    }
    profiler.end_frame();
    EXPECT_NE(find(profiler.zones(), "profile_test.once"), nullptr);
    for (int i = 0; i < 8; ++i) profiler.end_frame(); // window = 8
    EXPECT_EQ(find(profiler.zones(), "profile_test.once"), nullptr);
}

TEST_F(FrameProfilerTest, FullBufferDropsAndCounts) {
    const auto z = intern_profile_name("profile_test.many");
    for (int i = 0; i < 70; ++i) {
        const ProfileScope s(z);
    }
    profiler.end_frame();
    EXPECT_EQ(profiler.last_frame_scopes().size(), 64U);
    EXPECT_EQ(profiler.last_frame_dropped(), 6U);
}

TEST_F(FrameProfilerTest, CountersKeepTheLatestValue) {
    const auto draws = intern_profile_name("profile_test.draws");
    profiler.set_counter(draws, 10.0);
    profiler.set_counter(draws, 12.0);
    const auto counters = profiler.counters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].name, "profile_test.draws");
    EXPECT_DOUBLE_EQ(counters[0].value, 12.0);
    profiler.reset();
    EXPECT_TRUE(profiler.counters().empty());
    EXPECT_TRUE(profiler.zones().empty());
}

TEST(FrameProfiler, UnboundThreadsRecordNothing) {
    FrameProfiler profiler;
    const auto z = intern_profile_name("profile_test.unbound");
    {
        const ProfileScope s(z); // this thread is not bound
    }
    profiler.end_frame();
    EXPECT_TRUE(profiler.last_frame_scopes().empty());

    // A bound profiler ignores other threads' scopes.
    profiler.bind_current_thread();
    std::thread t([z] { const ProfileScope s(z); });
    t.join();
    profiler.end_frame();
    EXPECT_TRUE(profiler.last_frame_scopes().empty());
    profiler.unbind_current_thread();
}

TEST(FrameProfiler, SortZones) {
    std::vector<ZoneSummary> zones(3);
    zones[0].name = "b";
    zones[0].cpu.avg = 1.0F;
    zones[0].gpu.avg = 5.0F;
    zones[1].name = "a";
    zones[1].cpu.avg = 3.0F;
    zones[2].name = "c";
    zones[2].cpu.avg = 2.0F;
    zones[2].gpu.avg = 0.5F;

    engine::core::sort_zones(zones, ZoneSort::cost);
    EXPECT_EQ(zones[0].name, "b"); // its GPU time dominates
    EXPECT_EQ(zones[1].name, "a");
    engine::core::sort_zones(zones, ZoneSort::cpu_avg);
    EXPECT_EQ(zones[0].name, "a");
    engine::core::sort_zones(zones, ZoneSort::cpu_avg, /*ascending=*/true);
    EXPECT_EQ(zones[0].name, "b");
    engine::core::sort_zones(zones, ZoneSort::name, /*ascending=*/true);
    EXPECT_EQ(zones[0].name, "a");
    EXPECT_EQ(zones[2].name, "c");
}

TEST(ProfileMacros, FeedTheProcessProfiler) {
    FrameProfiler& global = engine::core::profiler();
    global.reset();
    global.bind_current_thread();
    {
        QUAT_ZONE_N("profile_test.macro_zone");
        QUAT_PLOT("profile_test.macro_plot", 42);
    }
    {
        QUAT_ZONE_C("profile_test.macro_system", ProfileCategory::system);
    }
    QUAT_FRAME_MARK();
    global.unbind_current_thread();

    const auto zones = global.zones();
    ASSERT_NE(find(zones, "profile_test.macro_zone"), nullptr);
    const ZoneSummary* sys = find(zones, "profile_test.macro_system");
    ASSERT_NE(sys, nullptr);
    EXPECT_EQ(sys->category, ProfileCategory::system);
    const auto counters = global.counters();
    const auto it = std::ranges::find_if(
        counters, [](const auto& c) { return c.name == "profile_test.macro_plot"; });
    ASSERT_NE(it, counters.end());
    EXPECT_DOUBLE_EQ(it->value, 42.0);
    global.reset();
}

} // namespace
