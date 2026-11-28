// CpuTileJobs — the GPU tile source's CPU fallback (issue #78): a fallback
// generation keeps the request's cancel token, cancel_all() stops every job,
// and destruction never waits out a full generation.

#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <thread>

#include <gtest/gtest.h>

#include "engine/terrain/cpu_tile_jobs.hpp"
#include "engine/terrain/generator.hpp"

namespace {

namespace t = engine::terrain;
using namespace std::chrono_literals;

// A worker finishes just after fulfilling its promise: reap until it has.
void expect_reaped(t::CpuTileJobs& jobs) {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    jobs.reap();
    while (jobs.size() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        jobs.reap();
    }
    EXPECT_EQ(jobs.size(), 0U);
}

// Hours of grid erosion if nothing stopped it.
[[nodiscard]] t::TerrainParams endless_params() {
    t::TerrainParams p;
    p.resolution = 129;
    p.tile_size_m = 256.0F;
    p.height_m = 40.0F;
    p.octaves = 4;
    p.erosion_model = t::ErosionModel::grid;
    p.grid.iterations = 50'000'000;
    return p;
}

[[nodiscard]] t::TerrainParams quick_params() {
    t::TerrainParams p = endless_params();
    p.resolution = 33;
    p.grid.iterations = 4;
    return p;
}

TEST(TerrainCpuTileJobs, DeliversTheCpuReference) {
    t::CpuTileJobs jobs;
    std::promise<t::Heightmap> promise;
    auto result = promise.get_future();
    jobs.start(quick_params(), std::move(promise), t::make_cancel_token());
    ASSERT_EQ(result.wait_for(60s), std::future_status::ready);
    const t::Heightmap map = result.get();
    ASSERT_TRUE(map.valid());
    EXPECT_EQ(map.heights, t::generate(quick_params()).heights);
    expect_reaped(jobs);
}

TEST(TerrainCpuTileJobs, RequestersTokenStopsTheGeneration) {
    t::CpuTileJobs jobs;
    const t::CancelToken token = t::make_cancel_token();
    std::promise<t::Heightmap> promise;
    auto result = promise.get_future();
    jobs.start(endless_params(), std::move(promise), token);
    EXPECT_EQ(result.wait_for(50ms), std::future_status::timeout);

    token->store(true); // the streamer moved on
    ASSERT_EQ(result.wait_for(30s), std::future_status::ready)
        << "the fallback ignored the request's cancel token";
    EXPECT_FALSE(result.get().valid());
    expect_reaped(jobs);
}

TEST(TerrainCpuTileJobs, CancelAllAndDestructionDoNotWaitForTheGeneration) {
    std::future<t::Heightmap> tokenless;
    std::future<t::Heightmap> tokened;
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::optional<t::CpuTileJobs> jobs;
        jobs.emplace();
        std::promise<t::Heightmap> a;
        tokenless = a.get_future();
        jobs->start(endless_params(), std::move(a), nullptr); // gets a token of its own
        std::promise<t::Heightmap> b;
        tokened = b.get_future();
        jobs->start(endless_params(), std::move(b), t::make_cancel_token());
        EXPECT_EQ(jobs->size(), 2U);

        jobs->cancel_all(); // GpuTileSource::abandon
        ASSERT_EQ(tokenless.wait_for(30s), std::future_status::ready);
        ASSERT_EQ(tokened.wait_for(30s), std::future_status::ready);
        EXPECT_FALSE(tokenless.get().valid());
        EXPECT_FALSE(tokened.get().valid());

        // A job started after cancel_all, then torn down with the set.
        std::promise<t::Heightmap> c;
        auto late = c.get_future();
        jobs->start(endless_params(), std::move(c), nullptr);
        jobs.reset(); // ~GpuTileSource: cancels, then joins
        ASSERT_EQ(late.wait_for(0s), std::future_status::ready) << "destruction did not join";
        EXPECT_FALSE(late.get().valid());
    }
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 30s);
}

TEST(TerrainCpuTileJobs, CancelledBeforeStartingReturnsAtOnce) {
    const t::CancelToken token = t::make_cancel_token();
    token->store(true);
    t::TerrainParams huge = endless_params();
    huge.resolution = 8193; // even the noise field would take seconds
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(t::generate(huge, token.get()).valid());
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
}

} // namespace
