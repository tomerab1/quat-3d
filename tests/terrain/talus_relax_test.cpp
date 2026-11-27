// Talus relaxation (issue #69): the multigrid cascade that runs thermal
// erosion to its steady state after the grid simulation. The CPU reference
// of the GPU relax_* kernels (tests/render/terrain_synth_test.cpp compares
// the two).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine/terrain/generator.hpp"
#include "engine/terrain/talus_relax.hpp"

namespace {

namespace t = engine::terrain;

// Share of neighbour pairs (8-neighbourhood, inside `margin` texels of the
// border) whose slope exceeds `factor` x talus, and the steepest slope.
struct Steepness {
    double share = 0.0;
    float  max_slope = 0.0F;
};
[[nodiscard]] Steepness steepness(const std::vector<float>& h, std::uint32_t res, float cell,
                                  float talus, float factor, std::uint32_t margin = 0) {
    std::uint64_t pairs = 0;
    std::uint64_t steep = 0;
    float worst = 0.0F;
    const auto r = static_cast<std::int32_t>(res);
    const auto m = static_cast<std::int32_t>(margin);
    for (std::int32_t z = m; z < r - m; ++z) {
        for (std::int32_t x = m; x < r - m; ++x) {
            for (const auto [dx, dz] : {std::pair{1, 0}, std::pair{0, 1}, std::pair{1, 1},
                                        std::pair{1, -1}}) {
                const std::int32_t nx = x + dx;
                const std::int32_t nz = z + dz;
                if (nx < m || nz < m || nx >= r - m || nz >= r - m) continue;
                const float dist = (dx != 0 && dz != 0) ? cell * std::sqrt(2.0F) : cell;
                const float slope =
                    std::abs(h[static_cast<std::size_t>(nz) * res + static_cast<std::size_t>(nx)] -
                             h[static_cast<std::size_t>(z) * res + static_cast<std::size_t>(x)]) /
                    dist;
                ++pairs;
                steep += slope > factor * talus ? 1 : 0;
                worst = std::max(worst, slope);
            }
        }
    }
    return {static_cast<double>(steep) / static_cast<double>(std::max<std::uint64_t>(pairs, 1)),
            worst};
}

[[nodiscard]] double sum(const std::vector<float>& h) {
    double s = 0.0;
    for (const float v : h) s += v;
    return s;
}

TEST(TerrainTalusRelax, PyramidHalvesDownToTheCoarsestLevel) {
    const t::TalusPyramid p = t::talus_pyramid(513, 2.0F, 1.0F);
    const std::uint32_t expected[] = {513, 257, 129, 65, 33, 17, 9};
    ASSERT_EQ(p.levels.size(), std::size(expected));
    std::size_t floats = 2ULL * 513 * 513;
    for (std::size_t k = 0; k < p.levels.size(); ++k) {
        EXPECT_EQ(p.levels[k].resolution, expected[k]);
        EXPECT_FLOAT_EQ(p.levels[k].limit_axis, 2.0F * static_cast<float>(1U << k));
        if (k > 0) floats += 3ULL * expected[k] * expected[k];
    }
    EXPECT_EQ(p.floats, floats);
    EXPECT_LT(p.floats, 31ULL * 513 * 513 / 10) << "~12 B a texel (terrain budget)";
    // Even resolutions keep their last texel: 100 -> 51 -> 26 -> 14 -> 8.
    const t::TalusPyramid even = t::talus_pyramid(100, 1.0F, 1.0F);
    ASSERT_EQ(even.levels.size(), 5U);
    EXPECT_EQ(even.levels.back().resolution, 8U);
    // A grid at or under the coarsest size is relaxed in place.
    EXPECT_EQ(t::talus_pyramid(t::talus_relax_coarsest, 1.0F, 1.0F).levels.size(), 1U);
    // Each cycle: restrict every coarse level, solve the coarsest, smooth
    // every finer level and prolong into it.
    const std::size_t coarse = p.levels.size() - 1;
    EXPECT_EQ(t::talus_schedule(p, 2).size(),
              2 * (coarse + t::talus_relax_coarsest_sweeps + coarse * t::talus_relax_smoothing +
                   coarse));
}

TEST(TerrainTalusRelax, ColumnSettlesIntoATalusConeConservingMaterial) {
    constexpr std::uint32_t n = 65;
    std::vector<float> h(n * n, 0.0F);
    h[32 * n + 32] = 40.0F; // a 40 m spike on a 1 m grid
    ASSERT_TRUE(t::relax_talus(h, n, 1.0F, 0.6F, 3));
    EXPECT_LT(h[32 * n + 32], 6.0F);
    EXPECT_NEAR(sum(h), 40.0, 1e-3) << "relaxation must conserve material";
    const Steepness s = steepness(h, n, 1.0F, 0.6F, 1.1F);
    EXPECT_EQ(s.share, 0.0) << "steepest slope " << s.max_slope;
}

// The case the interleaved thermal steps cannot fix: the interior of a
// uniform 60° ramp gives as much as it receives, so it only relaxes from the
// crest and toe inward, ~N² steps for N cells.
TEST(TerrainTalusRelax, UniformSteepRampComesDownToTheTalusAngle) {
    constexpr std::uint32_t n = 129;
    constexpr float cell = 2.0F;
    constexpr float talus = 1.0F; // 45°
    std::vector<float> ramp(n * n);
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            ramp[z * n + x] = std::tan(60.0F * 3.14159265F / 180.0F) * cell * static_cast<float>(x);
        }
    }
    const Steepness before = steepness(ramp, n, cell, talus, 1.1F);
    ASSERT_GT(before.share, 0.3);

    // As many plain thermal sweeps as the cascade costs barely touch it.
    std::vector<float> local = ramp;
    t::GridErosionParams thermal;
    thermal.iterations = 40;
    thermal.rain = 0.0F;
    thermal.talus = talus;
    thermal.thermal_rate = 1.0F;
    thermal.relax_cycles = 0;
    ASSERT_TRUE(t::erode_grid(local, n, cell, thermal));
    EXPECT_GT(steepness(local, n, cell, talus, 1.1F).share, 0.25);

    std::vector<float> relaxed = ramp;
    ASSERT_TRUE(t::relax_talus(relaxed, n, cell, talus, 3));
    const Steepness after = steepness(relaxed, n, cell, talus, 1.1F);
    EXPECT_LT(after.share, 0.01) << "steepest slope " << after.max_slope;
    EXPECT_NEAR(sum(relaxed) / (n * n), sum(ramp) / (n * n), 0.01 * 220.0);
}

TEST(TerrainTalusRelax, SurfaceWithinTheTalusIsAFixedPoint) {
    constexpr std::uint32_t n = 97;
    std::vector<float> h(n * n);
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            h[z * n + x] = 3.0F * std::sin(0.11F * static_cast<float>(x)) *
                           std::cos(0.07F * static_cast<float>(z));
        }
    }
    const std::vector<float> before = h;
    ASSERT_TRUE(t::relax_talus(h, n, 1.0F, 1.0F, 4)); // slopes <= 0.33 + 0.21
    EXPECT_EQ(h, before); // (a -0 may come back +0)
}

TEST(TerrainTalusRelax, ZeroCyclesAndCancellationLeaveTheFieldAlone) {
    constexpr std::uint32_t n = 33;
    std::vector<float> h(n * n, 0.0F);
    h[16 * n + 16] = 20.0F;
    const std::vector<float> before = h;
    ASSERT_TRUE(t::relax_talus(h, n, 1.0F, 0.5F, 0));
    EXPECT_EQ(h, before);
    std::atomic<bool> cancel{true};
    EXPECT_FALSE(t::relax_talus(h, n, 1.0F, 0.5F, 3, &cancel));
    EXPECT_EQ(h, before);
}

// The issue's measurement, as a CPU reference test: mountain terrain after
// grid erosion. Without the relaxation most of the steep faces stay steeper
// than the talus angle; with it (the default) they come down to it.
TEST(TerrainTalusRelax, ErodedMountainsSettleToTheTalusAngle) {
    t::TerrainParams p; // the mountain-forest scene's look at half the size
    p.seed = 29;
    p.resolution = 257;
    p.tile_size_m = 1000.0F;
    p.height_m = 450.0F;
    p.base_frequency = 1.6F;
    p.gain = 0.55F;
    p.erosion_model = t::ErosionModel::grid;
    p.grid.iterations = 60;
    p.grid.talus = 1.0F;
    p.grid.rain = 0.015F;
    p.grid.capacity = 1.0F;
    p.grid.erode_rate = 0.4F;
    p.grid.thermal_rate = 0.3F;
    const float cell = p.tile_size_m / static_cast<float>(p.resolution - 1);
    // The fade band blends back to the noise field: judge the interior.
    const std::uint32_t margin = 4 + static_cast<std::uint32_t>(0.05F * 257.0F) + 2;

    t::TerrainParams unrelaxed = p;
    unrelaxed.grid.relax_cycles = 0;
    const Steepness raw = steepness(t::generate(unrelaxed).heights, p.resolution, cell,
                                    p.grid.talus, 1.1F, margin);
    EXPECT_EQ(p.grid.relax_cycles, 3U) << "relaxation is on by default";
    const t::Heightmap relaxed = t::generate(p);
    ASSERT_TRUE(relaxed.valid());
    const Steepness settled = steepness(relaxed.heights, p.resolution, cell, p.grid.talus, 1.1F,
                                        margin);
    std::printf("[talus] pairs steeper than 1.1 x talus: %.2f%% -> %.3f%% (steepest %.2f -> %.2f)\n",
                100.0 * raw.share, 100.0 * settled.share, static_cast<double>(raw.max_slope),
                static_cast<double>(settled.max_slope));
    EXPECT_GT(raw.share, 0.05);
    EXPECT_LT(settled.share, 0.01);
}

} // namespace
