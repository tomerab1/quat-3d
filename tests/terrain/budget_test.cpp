// Terrain budget (issue #67): the combined memory of the tiles a
// configuration keeps loaded, plus the tile being generated, and droplet
// work across a streamed window are bounded as a whole.

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "engine/terrain/budget.hpp"
#include "engine/terrain/generator.hpp"

namespace {

namespace t = engine::terrain;
using Kind = t::TerrainBudgetProblem::Kind;

[[nodiscard]] t::TerrainParams params(std::uint32_t resolution, t::ErosionModel model) {
    t::TerrainParams p;
    p.resolution = resolution;
    p.erosion_model = model;
    return p;
}

TEST(TerrainBudget, CountsTheStreamedWindow) {
    EXPECT_EQ(t::terrain_tile_count(false, 8), 1U);
    EXPECT_EQ(t::terrain_tile_count(true, 0), 1U);
    EXPECT_EQ(t::terrain_tile_count(true, 1), 9U);
    EXPECT_EQ(t::terrain_tile_count(true, 8), 289U);
    EXPECT_EQ(t::terrain_tile_count(true, -3), 1U);
}

TEST(TerrainBudget, FootprintCountsEveryCopyOfEveryTile) {
    const t::TerrainParams grid = params(1025, t::ErosionModel::grid);
    const t::TerrainFootprint f = t::terrain_footprint(grid, 9);
    const std::uint64_t texels = 1025ULL * 1025ULL;
    EXPECT_EQ(f.tiles, 9U);
    EXPECT_EQ(f.resident_bytes, 9 * texels * t::terrain_resident_bytes_per_texel);
    EXPECT_EQ(f.scratch_bytes, texels * t::terrain_grid_scratch_bytes_per_texel);
    EXPECT_EQ(f.total(), f.resident_bytes + f.scratch_bytes);

    // Without erosion nothing but the tiles themselves is needed.
    t::TerrainParams flat = grid;
    flat.grid.iterations = 0;
    EXPECT_EQ(t::terrain_footprint(flat, 1).scratch_bytes, 0U);
    // The issue's case: 289 tiles of 8193² is ~78 GB of heightmaps alone.
    const t::TerrainParams huge = params(8193, t::ErosionModel::droplets);
    EXPECT_GT(t::terrain_footprint(huge, 289).resident_bytes, 289ULL * 8193 * 8193 * 4);
}

TEST(TerrainBudget, RejectsTheIssuesConfigurationsAndKeepsTheSupportedOnes) {
    const t::TerrainParams huge = params(8193, t::ErosionModel::droplets);
    const auto over = t::terrain_budget_problem(huge, true, 8);
    ASSERT_TRUE(over.has_value());
    EXPECT_EQ(over->kind, Kind::memory);
    EXPECT_NE(over->message.find("the limit is 2.00 GiB"), std::string::npos) << over->message;
    EXPECT_FALSE(t::terrain_budget_problem(huge, false, 8)) << "one 8193² tile must fit";
    EXPECT_TRUE(t::terrain_budget_problem(params(4097, t::ErosionModel::droplets), true, 4));

    // Defaults, the editor's widest window and grid erosion's largest tile.
    const t::TerrainParams def = params(1025, t::ErosionModel::grid);
    EXPECT_FALSE(t::terrain_budget_problem(def, false, 1));
    EXPECT_FALSE(t::terrain_budget_problem(def, true, 1));
    EXPECT_FALSE(t::terrain_budget_problem(def, true, 4));
    t::TerrainParams big = params(t::max_grid_erosion_resolution, t::ErosionModel::grid);
    big.grid.iterations = 1000;
    EXPECT_FALSE(t::terrain_budget_problem(big, true, 2));
    EXPECT_TRUE(t::terrain_budget_problem(big, true, 3));

    // A caller-chosen budget.
    EXPECT_TRUE(t::terrain_budget_problem(def, true, 1, 64ULL << 20U));
}

TEST(TerrainBudget, BoundsDropletsAcrossTheWindow) {
    t::TerrainParams p = params(257, t::ErosionModel::droplets);
    p.erosion_droplets = 5'000'000;
    EXPECT_FALSE(t::terrain_budget_problem(p, true, 1)); // 9 x 5 M = 45 M
    p.erosion_droplets = 6'000'000;
    const auto over = t::terrain_budget_problem(p, true, 1); // 9 x 6 M = 54 M
    ASSERT_TRUE(over.has_value());
    EXPECT_EQ(over->kind, Kind::droplets);
    EXPECT_NE(over->message.find("droplets <= 5555555"), std::string::npos) << over->message;
    EXPECT_FALSE(t::terrain_budget_problem(p, false, 1)) << "a single tile keeps its own cap";
    p.erosion_model = t::ErosionModel::grid; // the droplet count is unused
    EXPECT_FALSE(t::terrain_budget_problem(p, true, 1));
}

} // namespace
