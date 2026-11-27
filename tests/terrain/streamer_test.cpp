// Terrain streaming: the window fills, follows the camera, and adjacent tiles
// share bit-identical edges (the seam guarantee).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "engine/terrain/streamer.hpp"

namespace {

using engine::terrain::Heightmap;
using engine::terrain::TerrainParams;
using engine::terrain::TerrainStreamer;

[[nodiscard]] TerrainParams small_tiles() {
    TerrainParams base;
    base.resolution = 65;
    base.tile_size_m = 64.0F;
    base.height_m = 10.0F;
    base.octaves = 4;
    base.erosion_droplets = 2'000; // exercises the edge fade
    return base;
}

[[nodiscard]] bool has_tile(const std::vector<glm::ivec2>& tiles, glm::ivec2 c) {
    return std::find(tiles.begin(), tiles.end(), c) != tiles.end();
}

TEST(TerrainStreaming, BlockingUpdateFillsTheWindow) {
    TerrainStreamer streamer(small_tiles(), 1);
    const TerrainStreamer::Events events = streamer.update(glm::vec2(0.0F), /*block=*/true);
    EXPECT_EQ(streamer.maps().size(), 9U) << "expected a 3x3 window";
    EXPECT_EQ(events.ready.size(), 9U);
    EXPECT_TRUE(events.removed.empty());
}

TEST(TerrainStreaming, AdjacentTilesShareBitIdenticalEdges) {
    TerrainStreamer streamer(small_tiles(), 1);
    (void)streamer.update(glm::vec2(0.0F), /*block=*/true);
    ASSERT_TRUE(streamer.maps().contains(glm::ivec2(0, 0)));
    ASSERT_TRUE(streamer.maps().contains(glm::ivec2(1, 0)));

    // Tile (0,0)'s +x edge must equal tile (1,0)'s -x edge bit-exactly.
    const Heightmap& a = streamer.maps().at(glm::ivec2(0, 0));
    const Heightmap& b = streamer.maps().at(glm::ivec2(1, 0));
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    ASSERT_EQ(a.resolution, b.resolution);
    const std::uint32_t res = a.resolution;
    for (std::uint32_t z = 0; z < res; ++z) {
        const float ha = a.heights[static_cast<std::size_t>(z) * res + (res - 1)];
        const float hb = b.heights[static_cast<std::size_t>(z) * res + 0];
        ASSERT_EQ(std::memcmp(&ha, &hb, sizeof(float)), 0)
            << "tiles diverge at the seam, row " << z << ": " << ha << " vs " << hb;
    }
}

TEST(TerrainStreaming, MovingOneTileEastSwapsOneColumn) {
    const TerrainParams base = small_tiles();
    TerrainStreamer streamer(base, 1);
    (void)streamer.update(glm::vec2(0.0F), /*block=*/true);

    const TerrainStreamer::Events events =
        streamer.update(glm::vec2(base.tile_size_m, 0.0F), /*block=*/true);
    EXPECT_EQ(streamer.maps().size(), 9U) << "window size changed after a move";
    EXPECT_EQ(events.removed.size(), 3U);
    EXPECT_EQ(events.ready.size(), 3U);
    for (int z = -1; z <= 1; ++z) {
        EXPECT_TRUE(has_tile(events.ready, {2, z})) << "column x=2 not loaded, z=" << z;
        EXPECT_TRUE(has_tile(events.removed, {-1, z})) << "column x=-1 not dropped, z=" << z;
    }
    EXPECT_TRUE(streamer.maps().contains(glm::ivec2(2, 0)));
    EXPECT_FALSE(streamer.maps().contains(glm::ivec2(-1, 0)));
}

// Issue #38: changing the stream radius resizes the window around the tiles
// already loaded instead of regenerating the world.
TEST(TerrainStreaming, RadiusChangeKeepsLoadedTiles) {
    TerrainStreamer streamer(small_tiles(), 1);
    (void)streamer.update(glm::vec2(0.0F), /*block=*/true);
    const float* centre = streamer.maps().at(glm::ivec2(0, 0)).heights.data();

    streamer.set_radius(2);
    TerrainStreamer::Events events = streamer.update(glm::vec2(0.0F), /*block=*/true);
    EXPECT_EQ(streamer.maps().size(), 25U);
    EXPECT_EQ(events.ready.size(), 16U) << "only the new ring is generated";
    EXPECT_TRUE(events.removed.empty());
    EXPECT_EQ(streamer.maps().at(glm::ivec2(0, 0)).heights.data(), centre)
        << "the loaded tile was kept, not regenerated";

    streamer.set_radius(0);
    events = streamer.update(glm::vec2(0.0F), /*block=*/true);
    EXPECT_EQ(streamer.maps().size(), 1U);
    EXPECT_EQ(events.removed.size(), 24U);
    EXPECT_TRUE(events.ready.empty());
}

TEST(TerrainParamsShape, OnlyGenerationFieldsChangeTheShape) {
    const TerrainParams a = small_tiles();
    TerrainParams b = a;
    b.world_tile = glm::ivec2(3, -2); // set per streamed tile
    EXPECT_TRUE(engine::terrain::same_shape(a, b));
    b.seed += 1;
    EXPECT_FALSE(engine::terrain::same_shape(a, b));
    b = a;
    b.grid.talus = 0.7F;
    EXPECT_FALSE(engine::terrain::same_shape(a, b));
}

} // namespace
