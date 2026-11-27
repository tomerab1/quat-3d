// profile.* remote commands (R4.3): parameter validation, the rolling frame
// profile, the capture -> result flow across frames, counters and reset.

#include <gtest/gtest.h>

#include <vector>

#include "engine/core/frame_profiler.hpp"
#include "world_fixture.hpp"

namespace quat::test {
namespace {

using engine::core::ProfileCategory;
using engine::remote::rpc_code::invalid_params;
using engine::remote::rpc_code::not_found;

class WorldProfile : public WorldCommands {
protected:
    engine::core::FrameProfiler profiler{engine::core::FrameProfiler::Config{.window = 16}};
    engine::core::ProfileNameId pass =
        engine::core::intern_profile_name("world_profile_test.gbuffer", ProfileCategory::pass);
    engine::core::ProfileNameId system =
        engine::core::intern_profile_name("world_profile_test.physics", ProfileCategory::system);

    void SetUp() override {
        services.profiler = &profiler;
        services.gpu_memory = [] {
            return std::vector<engine::remote::GpuMemoryHeap>{
                {.heap = 0,
                 .device_local = true,
                 .size = 8ULL << 30U,
                 .budget = 6ULL << 30U,
                 .usage = 512ULL << 20U,
                 .allocated = 256ULL << 20U,
                 .used = 200ULL << 20U,
                 .allocations = 42},
                {.heap = 1, .device_local = false, .size = 16ULL << 30U}};
        };
        register_commands();
        profiler.bind_current_thread();
    }
    void TearDown() override { profiler.unbind_current_thread(); }

    // One frame with both zones on the CPU and the pass on the GPU; its GPU
    // timings arrive after it closed, as in the frame loop.
    void frame() {
        const std::uint64_t index = profiler.frame_index();
        {
            const engine::core::ProfileScope s(system);
        }
        {
            const engine::core::ProfileScope p(pass);
        }
        profiler.end_frame();
        const std::vector<engine::core::GpuZoneTiming> gpu{{pass, 3.0F}};
        profiler.submit_gpu_frame(index, gpu, 4.0F);
    }
};

TEST_F(WorldProfile, FrameReportsZonesWithCpuAndGpu) {
    for (int i = 0; i < 4; ++i) frame();
    const Json out = ok("profile.frame", {{"kind", "pass"}});
    EXPECT_EQ(out["window"], 16);
    EXPECT_EQ(out["frame"], 4);
    EXPECT_EQ(out["frames"], 3); // the first frame has no start time
    ASSERT_TRUE(out["frame_ms"]["total"].is_object());
    EXPECT_TRUE(out["frame_ms"]["gpu"].is_object());
    EXPECT_DOUBLE_EQ(out["frame_ms"]["gpu"]["avg"].get<double>(), 4.0);
    ASSERT_EQ(out["zones"].size(), 1U); // only passes
    const Json& z = out["zones"][0];
    EXPECT_EQ(z["name"], "world_profile_test.gbuffer");
    EXPECT_EQ(z["kind"], "pass");
    EXPECT_DOUBLE_EQ(z["gpu"]["avg"].get<double>(), 3.0);
    EXPECT_TRUE(z["cpu"].is_object());
    EXPECT_EQ(z["calls"], 1);
    EXPECT_EQ(out["zones_matched"], 1);

    const Json all = ok("profile.frame", {{"sort", "name"}, {"limit", 1}});
    EXPECT_EQ(all["zones"].size(), 1U);
    EXPECT_GE(all["zones_matched"].get<int>(), 2);
}

TEST_F(WorldProfile, FrameValidatesItsParameters) {
    EXPECT_NE(err("profile.frame", {{"sort", "fastest"}}, invalid_params).find("sort"),
              std::string::npos);
    EXPECT_NE(err("profile.frame", {{"kind", "shader"}}, invalid_params).find("kind"),
              std::string::npos);
    err("profile.frame", {{"limit", 0}}, invalid_params);
    err("profile.frame", {{"limit", 201}}, invalid_params);
    EXPECT_NE(err("profile.frame", {{"window", 10}}, invalid_params).find("window"),
              std::string::npos);
}

TEST_F(WorldProfile, CaptureRunsAcrossFramesThenSummarises) {
    frame(); // the first frame closes without a frame time
    const Json started = ok("profile.capture", {{"frames", 3}, {"budget_ms", 1.0}, {"top", 2}});
    EXPECT_EQ(started["status"], "recording");
    const int id = started["id"].get<int>();

    frame();
    Json r = ok("profile.result", {{"id", id}});
    EXPECT_EQ(r["status"], "recording");
    EXPECT_EQ(r["frames_done"], 1);
    EXPECT_EQ(r["frames"], 3);

    frame();
    frame();
    r = ok("profile.result", {{"id", id}});
    ASSERT_EQ(r["status"], "done") << r.dump();
    EXPECT_EQ(r["frames"], 3);
    EXPECT_EQ(r["gpu_frames"], 3);
    EXPECT_LE(r["top_by_total"].size(), 2U);
    EXPECT_EQ(r["top_by_total"][0]["name"], "world_profile_test.gbuffer");
    EXPECT_DOUBLE_EQ(r["top_by_total"][0]["gpu"]["total"].get<double>(), 9.0);
    EXPECT_EQ(r["per_frame"]["total_ms"].size(), 3U);
    EXPECT_EQ(r["per_frame"]["gpu_ms"][0], 4.0);
    // Every frame's GPU time (4 ms) is over the 1 ms budget.
    EXPECT_EQ(r["spikes"]["count"], 3);
    EXPECT_EQ(r["spikes"]["worst"][0]["zones"][0]["on"], "gpu");
}

TEST_F(WorldProfile, CaptureAndResultValidateTheirParameters) {
    err("profile.capture", {{"frames", 0}}, invalid_params);
    err("profile.capture", {{"frames", 1001}}, invalid_params);
    err("profile.capture", {{"budget_ms", 0}}, invalid_params);
    err("profile.capture", {{"top", 51}}, invalid_params);
    err("profile.capture", {{"frame", 10}}, invalid_params);

    EXPECT_NE(err("profile.result", Json::object(), invalid_params).find("id"),
              std::string::npos);
    err("profile.result", {{"id", 0}}, invalid_params);
    err("profile.result", {{"id", "one"}}, invalid_params);
    EXPECT_NE(err("profile.result", {{"id", 99}}, not_found).find("no capture"),
              std::string::npos);

    ok("profile.capture", {{"frames", 10}});
    EXPECT_NE(err("profile.capture", {{"frames", 10}}, engine::remote::rpc_code::failed)
                  .find("busy"),
              std::string::npos);
    EXPECT_NE(err("profile.result", {{"id", 99}}, not_found).find("ids 1..1"),
              std::string::npos);
}

TEST_F(WorldProfile, CountersReportMemoryAndEntities) {
    profiler.set_counter(engine::core::intern_profile_name("draws"), 12.0);
    const entt::entity e = reg().create();
    reg().emplace<engine::scene::Transform>(e);
    reg().emplace<engine::scene::PointLight>(e);
    const Json out = ok("profile.counters");
    EXPECT_EQ(out["counters"]["draws"], 12.0);
    ASSERT_TRUE(out["gpu_memory"].is_object());
    EXPECT_DOUBLE_EQ(out["gpu_memory"]["device_local_usage_mib"].get<double>(), 512.0);
    EXPECT_DOUBLE_EQ(out["gpu_memory"]["device_local_budget_mib"].get<double>(), 6144.0);
    ASSERT_EQ(out["gpu_memory"]["heaps"].size(), 2U);
    EXPECT_EQ(out["gpu_memory"]["heaps"][0]["allocations"], 42);
    EXPECT_EQ(out["entities"]["total"], 1);
    EXPECT_EQ(out["entities"]["point_lights"], 1);
    EXPECT_EQ(out["entities"]["meshes"], 0);
    err("profile.counters", {{"heap", 0}}, invalid_params);
}

TEST_F(WorldProfile, ResetClearsStatisticsAndCaptures) {
    frame();
    frame();
    ok("profile.capture", {{"frames", 5}});
    const Json out = ok("profile.reset");
    EXPECT_EQ(out["reset"], true);
    const Json after = ok("profile.frame");
    EXPECT_TRUE(after["zones"].empty());
    EXPECT_TRUE(after["frame_ms"]["total"].is_null());
    ok("profile.capture", {{"frames", 5}}); // not busy any more
    err("profile.reset", {{"all", true}}, invalid_params);
}

class WorldProfileUnavailable : public WorldCommands {
protected:
    void SetUp() override { register_commands(); } // no profiler
};

TEST_F(WorldProfileUnavailable, EveryMethodSaysSo) {
    using engine::remote::rpc_code::unavailable;
    for (const char* m : {"profile.frame", "profile.counters", "profile.reset"}) {
        EXPECT_NE(err(m, Json::object(), unavailable).find("profiler"), std::string::npos) << m;
    }
    err("profile.capture", {{"frames", 5}}, unavailable);
    err("profile.result", {{"id", 1}}, unavailable);
}

} // namespace
} // namespace quat::test
