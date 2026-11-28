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

// Volume under the bilinear surface, in samples (the trapezoid rule): border
// samples count half, corners a quarter. What the relaxation conserves.
[[nodiscard]] double volume(const std::vector<float>& h, std::uint32_t n) {
    double v = 0.0;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const double wx = (x == 0 || x == n - 1) ? 0.5 : 1.0;
            const double wz = (z == 0 || z == n - 1) ? 0.5 : 1.0;
            v += wx * wz * static_cast<double>(h[static_cast<std::size_t>(z) * n + x]);
        }
    }
    return v;
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
    // Only odd grids nest: 97 -> 49 -> 25 -> 13 -> 7; 99 -> 50 stops there;
    // an even grid is relaxed in place.
    EXPECT_EQ(t::talus_pyramid(97, 1.0F, 1.0F).levels.back().resolution, 7U);
    EXPECT_EQ(t::talus_pyramid(99, 1.0F, 1.0F).levels.back().resolution, 50U);
    EXPECT_EQ(t::talus_pyramid(100, 1.0F, 1.0F).levels.size(), 1U);
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
    EXPECT_NEAR(volume(h, n), 40.0, 1e-3) << "relaxation must conserve material";
    const Steepness s = steepness(h, n, 1.0F, 0.6F, 1.1F);
    EXPECT_EQ(s.share, 0.0) << "steepest slope " << s.max_slope;
}

// PR #90 review: a narrow feature must never turn into a pit. Thermal
// erosion only moves material downhill, so nothing may leave the input's
// [min, max] — wherever the feature sits: even and odd texels (decimation
// aliased the even ones into coarse plateaus), borders and corners. And the
// volume is conserved there too.
TEST(TerrainTalusRelax, StaysWithinTheInputRangeAndConservesVolume) {
    struct Feature {
        const char*   name;
        std::uint32_t x, z, size;
        float         height;
    };
    for (const std::uint32_t n : {33U, 65U, 129U, 257U}) {
        const std::uint32_t mid = n / 2;
        const Feature features[] = {
            {"spike, even texel", mid, mid, 1, 50.0F},
            {"spike, odd texel", mid + 1, mid + 1, 1, 50.0F},
            {"3x3 block, odd corner", mid - 1, mid + 2, 3, 50.0F},
            {"spike on the border", 0, mid + 1, 1, 50.0F},
            {"column on the border", 0, mid, 3, 40.0F},
            {"spike in the corner", n - 1, n - 1, 1, 50.0F},
            {"block in the corner", 0, 0, 2, 40.0F},
            {"16x16 block", mid - 8, mid - 8, 16, 100.0F},
        };
        for (const Feature& f : features) {
            std::vector<float> h(static_cast<std::size_t>(n) * n, 3.0F); // base above 0
            for (std::uint32_t z = f.z; z < f.z + f.size && z < n; ++z) {
                for (std::uint32_t x = f.x; x < f.x + f.size && x < n; ++x) {
                    h[static_cast<std::size_t>(z) * n + x] = f.height;
                }
            }
            const double before = volume(h, n);
            ASSERT_TRUE(t::relax_talus(h, n, 1.0F, 0.5F, 3));
            const auto [lo, hi] = std::ranges::minmax(h);
            EXPECT_GE(lo, 3.0F) << n << "²: " << f.name << " dug a pit";
            EXPECT_LE(hi, f.height) << n << "²: " << f.name;
            EXPECT_NEAR(volume(h, n), before, 1e-5 * before) << n << "²: " << f.name;
        }
    }
}

// The same on rough fields: white noise is the worst case for aliasing.
TEST(TerrainTalusRelax, NoiseFieldsStayInRangeAndConserveVolume) {
    std::uint32_t state = 12345;
    const auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(state >> 8) * (1.0F / 16777216.0F);
    };
    for (const std::uint32_t n : {65U, 129U}) {
        std::vector<float> h(static_cast<std::size_t>(n) * n);
        for (float& v : h) v = 40.0F * next();
        const auto [lo, hi] = std::ranges::minmax(h);
        const double before = volume(h, n);
        ASSERT_TRUE(t::relax_talus(h, n, 1.0F, 0.5F, 3));
        const auto [out_lo, out_hi] = std::ranges::minmax(h);
        EXPECT_GE(out_lo, lo) << n;
        EXPECT_LE(out_hi, hi) << n;
        EXPECT_NEAR(volume(h, n), before, 1e-5 * before) << n;
        EXPECT_LT(steepness(h, n, 1.0F, 0.5F, 1.2F).share, 0.01) << n;
    }
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
    thermal.iterations = 100;
    thermal.rain = 0.0F;
    thermal.talus = talus;
    thermal.thermal_rate = 1.0F;
    thermal.relax_cycles = 0;
    ASSERT_TRUE(t::erode_grid(local, n, cell, thermal));
    EXPECT_GT(steepness(local, n, cell, talus, 1.2F).share, 0.25);
    EXPECT_GT(steepness(local, n, cell, talus, 1.2F).max_slope, 1.7F);

    std::vector<float> relaxed = ramp;
    ASSERT_TRUE(t::relax_talus(relaxed, n, cell, talus, 3));
    const Steepness after = steepness(relaxed, n, cell, talus, 1.2F);
    std::printf("[talus] 60° ramp: steepest %.3f -> %.3f x talus\n",
                static_cast<double>(before.max_slope), static_cast<double>(after.max_slope));
    EXPECT_EQ(after.share, 0.0) << "steepest slope " << after.max_slope;
    EXPECT_NEAR(volume(relaxed, n), volume(ramp, n), 1e-5 * volume(ramp, n));
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
