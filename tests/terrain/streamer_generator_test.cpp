// TerrainStreamer with a TileGenerator backend (R10.2): supported tiles go to
// the backend, unsupported ones and blocking updates stay on the CPU, and a
// backend that delivers nothing usable never wedges the window.

#include <chrono>
#include <cstdint>
#include <future>
#include <map>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine/terrain/streamer.hpp"
#include "engine/terrain/tile_generator.hpp"

namespace {

namespace t = engine::terrain;

// A backend the test drives by hand: generate() parks a promise per tile.
class ManualGenerator final : public t::TileGenerator {
public:
    bool accept = true;
    std::vector<t::TerrainParams> requests;
    std::map<std::pair<int, int>, std::promise<t::Heightmap>> pending;

    [[nodiscard]] bool supports(const t::TerrainParams&) const override { return accept; }

    [[nodiscard]] std::future<t::Heightmap> generate(const t::TerrainParams& params) override {
        requests.push_back(params);
        auto& p = pending[{params.world_tile.x, params.world_tile.y}];
        p = std::promise<t::Heightmap>();
        return p.get_future();
    }

    // Completes the tile with the CPU reference, tagged as a "GPU" map.
    void finish(const t::TerrainParams& params, std::uint64_t tag) {
        t::Heightmap map = t::generate(params);
        map.gpu_tile = tag;
        pending.at({params.world_tile.x, params.world_tile.y}).set_value(std::move(map));
    }
};

[[nodiscard]] t::TerrainParams tiny() {
    t::TerrainParams p;
    p.resolution = 33;
    p.tile_size_m = 32.0F;
    p.height_m = 5.0F;
    p.octaves = 3;
    p.erosion_droplets = 0;
    return p;
}

TEST(TerrainStreamingBackend, SupportedTilesGoToTheBackend) {
    ManualGenerator gen;
    t::TerrainStreamer streamer(tiny(), 0); // one tile
    streamer.set_generator(&gen);

    auto ev = streamer.update(glm::vec2(0.0F));
    ASSERT_EQ(gen.requests.size(), 1U);
    EXPECT_EQ(gen.requests[0].world_tile, glm::ivec2(0, 0));
    EXPECT_TRUE(ev.ready.empty()) << "tile ready before the backend delivered";

    gen.finish(gen.requests[0], 77);
    ev = streamer.update(glm::vec2(0.0F));
    ASSERT_EQ(ev.ready.size(), 1U);
    EXPECT_EQ(streamer.maps().at(glm::ivec2(0, 0)).gpu_tile, 77U);
    EXPECT_EQ(gen.requests.size(), 1U) << "a full window must not request more tiles";
}

TEST(TerrainStreamingBackend, UnsupportedTilesStayOnTheCpu) {
    ManualGenerator gen;
    gen.accept = false;
    t::TerrainStreamer streamer(tiny(), 0);
    streamer.set_generator(&gen);
    t::TerrainStreamer::Events ev;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (ev.ready.empty() && std::chrono::steady_clock::now() < deadline) {
        ev = streamer.update(glm::vec2(0.0F));
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // CPU worker running
    }
    ASSERT_EQ(ev.ready.size(), 1U);
    EXPECT_TRUE(gen.requests.empty());
    EXPECT_EQ(streamer.maps().at(glm::ivec2(0, 0)).gpu_tile, 0U);
}

TEST(TerrainStreamingBackend, BlockingUpdatesNeverWaitOnTheBackend) {
    ManualGenerator gen; // never finishes on its own, like a GPU between frames
    t::TerrainStreamer streamer(tiny(), 1);
    streamer.set_generator(&gen);
    (void)streamer.update(glm::vec2(0.0F)); // kicks one backend job
    ASSERT_EQ(gen.requests.size(), 1U);

    const auto ev = streamer.update(glm::vec2(0.0F), /*block=*/true);
    EXPECT_EQ(streamer.maps().size(), 9U);
    EXPECT_EQ(ev.ready.size(), 9U);
    for (const auto& [coord, map] : streamer.maps()) {
        EXPECT_EQ(map.gpu_tile, 0U) << "tile " << coord.x << "," << coord.y;
    }
    EXPECT_EQ(gen.requests.size(), 1U);
}

TEST(TerrainStreamingBackend, AnInvalidDeliveryIsRetried) {
    ManualGenerator gen;
    t::TerrainStreamer streamer(tiny(), 0);
    streamer.set_generator(&gen);
    (void)streamer.update(glm::vec2(0.0F));
    ASSERT_EQ(gen.requests.size(), 1U);
    gen.pending.begin()->second.set_value(t::Heightmap{}); // e.g. backend shut down

    auto ev = streamer.update(glm::vec2(0.0F));
    EXPECT_TRUE(ev.ready.empty());
    EXPECT_TRUE(streamer.maps().empty());
    ASSERT_EQ(gen.requests.size(), 2U) << "the tile was not requested again";
    gen.finish(gen.requests[1], 5);
    ev = streamer.update(glm::vec2(0.0F));
    EXPECT_EQ(ev.ready.size(), 1U);
}

} // namespace
