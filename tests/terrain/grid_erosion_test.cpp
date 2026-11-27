// Grid (pipe-model hydraulic + thermal) erosion — the CPU reference of the GPU
// synthesis pass (R10.2): determinism, the untouched border band (streaming
// seams), that it actually erodes, and the shared world-lattice sampling.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>

#include <gtest/gtest.h>

#include "engine/terrain/generator.hpp"
#include "engine/terrain/streamer.hpp"

namespace {

namespace t = engine::terrain;

[[nodiscard]] t::TerrainParams grid_params() {
    t::TerrainParams p;
    p.resolution = 97;
    p.tile_size_m = 192.0F;
    p.height_m = 50.0F;
    p.octaves = 5;
    p.erosion_model = t::ErosionModel::grid;
    p.grid.iterations = 80;
    return p;
}

[[nodiscard]] bool bit_identical(const t::Heightmap& a, const t::Heightmap& b) {
    return a.heights.size() == b.heights.size() &&
           std::memcmp(a.heights.data(), b.heights.data(), a.heights.size() * sizeof(float)) == 0;
}

[[nodiscard]] double total(const t::Heightmap& m) {
    double sum = 0.0;
    for (const float v : m.heights) sum += v;
    return sum;
}

TEST(TerrainGridErosion, SameParamsReproduceTheExactBits) {
    EXPECT_TRUE(bit_identical(t::generate(grid_params()), t::generate(grid_params())));
}

TEST(TerrainGridErosion, ErodesTheInteriorAndKeepsTheBorderBand) {
    const t::TerrainParams p = grid_params();
    t::TerrainParams flat = p;
    flat.grid.iterations = 0;
    const t::Heightmap eroded = t::generate(p);
    const t::Heightmap base = t::generate(flat);
    ASSERT_TRUE(eroded.valid());

    double moved = 0.0;
    const std::uint32_t n = p.resolution;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::size_t i = static_cast<std::size_t>(z) * n + x;
            if (t::grid_erosion_fade(x, z, n) <= 0.0F) {
                ASSERT_EQ(std::memcmp(&eroded.heights[i], &base.heights[i], sizeof(float)), 0)
                    << "border texel (" << x << "," << z << ") was eroded";
            }
            moved += std::abs(eroded.heights[i] - base.heights[i]);
        }
    }
    EXPECT_GT(moved / static_cast<double>(n * n), 1e-3) << "erosion moved no material";
    // Water carries material downhill but (rain/evaporation aside) mass is
    // conserved: the mean height barely changes.
    EXPECT_NEAR(total(eroded) / static_cast<double>(n * n),
                total(base) / static_cast<double>(n * n), 0.05 * p.height_m);
}

TEST(TerrainGridErosion, ThermalWeatheringRelaxesSteepSlopes) {
    // A single 20 m column on a flat 1 m grid: far steeper than the talus.
    std::vector<float> h(33 * 33, 0.0F);
    h[16 * 33 + 16] = 20.0F;
    t::GridErosionParams p;
    p.iterations = 40;
    p.rain = 0.0F; // thermal only
    t::erode_grid(h, 33, 1.0F, p);
    EXPECT_LT(h[16 * 33 + 16], 10.0F);
    EXPECT_GT(h[16 * 33 + 17], 0.5F);
    double sum = 0.0;
    for (const float v : h) sum += v;
    EXPECT_NEAR(sum, 20.0, 1e-3) << "thermal transfer must conserve material";
}

TEST(TerrainGridErosion, StreamedGridTilesShareBitIdenticalEdges) {
    t::TerrainParams base = grid_params();
    base.resolution = 65;
    base.tile_size_m = 128.0F;
    base.grid.iterations = 30;
    t::TerrainStreamer streamer(base, 1);
    (void)streamer.update(glm::vec2(0.0F), /*block=*/true);
    const t::Heightmap& a = streamer.maps().at(glm::ivec2(0, 0));
    const t::Heightmap& b = streamer.maps().at(glm::ivec2(0, 1));
    const std::uint32_t n = a.resolution;
    for (std::uint32_t x = 0; x < n; ++x) {
        const float ha = a.heights[static_cast<std::size_t>(n - 1) * n + x];
        const float hb = b.heights[x];
        ASSERT_EQ(std::memcmp(&ha, &hb, sizeof(float)), 0) << "column " << x;
    }
}

// Audit D3: a CPU generation stops at its next erosion step once cancelled
// (streamer teardown / app exit never waits out a long simulation).
TEST(TerrainGridErosion, CancelStopsTheSimulation) {
    t::TerrainParams p = grid_params();
    p.grid.iterations = 1'000'000; // hours if it ran
    std::atomic<bool> cancel{true};
    const auto t0 = std::chrono::steady_clock::now();
    const t::Heightmap map = t::generate(p, &cancel);
    EXPECT_FALSE(map.valid());
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(10));

    t::CancelToken token = t::make_cancel_token();
    auto job = t::generate_async(p, token);
    token->store(true);
    ASSERT_EQ(job.wait_for(std::chrono::seconds(30)), std::future_status::ready);
    EXPECT_FALSE(job.get().valid());

    t::TerrainParams droplets = grid_params();
    droplets.erosion_model = t::ErosionModel::droplets;
    droplets.erosion_droplets = 50'000'000;
    EXPECT_FALSE(t::generate(droplets, &cancel).valid());
}

TEST(TerrainGridErosion, LimitsBoundResolutionAndSteps) {
    t::TerrainParams p = grid_params();
    p.resolution = 1025;
    p.grid.iterations = t::max_grid_erosion_steps;
    EXPECT_TRUE(t::grid_erosion_within_limits(p));
    p.grid.iterations += 1;
    EXPECT_FALSE(t::grid_erosion_within_limits(p));
    p.grid.iterations = 10;
    p.resolution = t::max_grid_erosion_resolution + 2;
    EXPECT_FALSE(t::grid_erosion_within_limits(p));
    p.erosion_model = t::ErosionModel::droplets;
    EXPECT_TRUE(t::grid_erosion_within_limits(p));
}

TEST(TerrainGridErosion, NoiseIsSampledOnTheWorldTexelLattice) {
    t::TerrainParams p;
    p.resolution = 65;
    p.world_tile = glm::ivec2(-2, 3);
    EXPECT_EQ(t::world_texel(p, glm::ivec2(0, 0)), glm::ivec2(-128, 192));
    EXPECT_EQ(t::world_texel(p, glm::ivec2(64, 64)), glm::ivec2(-64, 256));
    EXPECT_FLOAT_EQ(t::noise_texel_scale(p), p.base_frequency / 64.0F);
    EXPECT_FALSE(t::erosion_enabled([] {
        t::TerrainParams q;
        q.erosion_model = t::ErosionModel::grid;
        q.grid.iterations = 0;
        return q;
    }()));
}

} // namespace
