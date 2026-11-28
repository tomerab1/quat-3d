// EditSync (G19.1): loaded tiles follow the edit list region by region, and a
// tile that loads later matches one that was edited in place.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "engine/terrain/edit_sync.hpp"
#include "engine/terrain/edits.hpp"

namespace {

using namespace engine::terrain;

Heightmap flat_tile(std::uint32_t res, float size, float h) {
    Heightmap m;
    m.resolution = res;
    m.tile_size_m = size;
    m.heights.assign(static_cast<std::size_t>(res) * res, h);
    m.min_height = h;
    m.max_height = h;
    return m;
}

TerrainStamp bump_at(glm::vec2 p, float amount) {
    TerrainStamp s;
    s.points = {p};
    s.radius = 4.0F;
    s.falloff = 4.0F;
    s.amount = amount;
    return s;
}

TEST(TerrainEditSync, FollowsTheListRegionByRegion) {
    Heightmap a = flat_tile(65, 64.0F, 1.0F); // tile (0,0): [-32, 32]
    Heightmap b = flat_tile(65, 64.0F, 1.0F); // tile (1,0): [32, 96]
    const std::vector<EditTile> tiles{{{0, 0}, &a}, {{1, 0}, &b}};
    EditSync sync;
    TerrainEditList list;

    auto seam = list.add(bump_at({32.0F, 0.0F}, 2.0F)); // on the shared edge
    ASSERT_TRUE(seam);
    EditSync::Result r = sync.sync(list.stamps(), list.revision(), tiles);
    EXPECT_EQ(r.tiles.size(), 2U);
    EXPECT_EQ(a.heights[32U * 65U + 64U], 3.0F);
    EXPECT_EQ(b.heights[32U * 65U + 0U], 3.0F);
    EXPECT_TRUE(sync.sync(list.stamps(), list.revision(), tiles).empty()); // same revision

    auto inside = list.add(bump_at({-20.0F, -20.0F}, 1.0F)); // tile a only
    ASSERT_TRUE(inside);
    r = sync.sync(list.stamps(), list.revision(), tiles);
    ASSERT_EQ(r.tiles.size(), 1U);
    EXPECT_EQ(r.tiles[0].coord, glm::ivec2(0, 0));
    EXPECT_EQ(a.edit_revision, list.revision());
    EXPECT_EQ(b.edit_revision, list.revision());

    // A tile loaded now (tile (1,0) again, regenerated) equals the edited one.
    Heightmap reloaded = flat_tile(65, 64.0F, 1.0F);
    EXPECT_FALSE(sync.prepare_tile({{1, 0}, &reloaded}).empty());
    EXPECT_EQ(reloaded.heights, b.heights);
    EXPECT_EQ(reloaded.paint, b.paint);
    EXPECT_EQ(reloaded.edit_cells, b.edit_cells);
    EXPECT_TRUE(sync.prepare_tile({{1, 0}, &reloaded}).empty()); // already current

    ASSERT_TRUE(list.remove(*seam));
    r = sync.sync(list.stamps(), list.revision(), tiles);
    EXPECT_EQ(r.tiles.size(), 2U);
    EXPECT_EQ(b.heights[32U * 65U], 1.0F);
}

} // namespace
