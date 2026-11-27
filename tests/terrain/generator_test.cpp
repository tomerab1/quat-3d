// Procedural terrain generation: bounds, determinism, seed/warp sensitivity,
// erosion smoothing, and the normal helper.

#include <cmath>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "engine/terrain/generator.hpp"

namespace {

using engine::terrain::generate;
using engine::terrain::Heightmap;
using engine::terrain::TerrainParams;

// Exact (bitwise) equality of two height fields — determinism means the same
// bits, not merely values that compare equal.
[[nodiscard]] bool bit_identical(const Heightmap& a, const Heightmap& b) {
    return a.heights.size() == b.heights.size() &&
           std::memcmp(a.heights.data(), b.heights.data(), a.heights.size() * sizeof(float)) == 0;
}

[[nodiscard]] float mean_abs_gradient(const Heightmap& m) {
    double total = 0.0;
    const auto res = static_cast<std::int32_t>(m.resolution);
    for (std::int32_t z = 0; z < res; ++z) {
        for (std::int32_t x = 0; x + 1 < res; ++x) {
            total += std::abs(m.at(x + 1, z) - m.at(x, z));
        }
    }
    return static_cast<float>(total / (static_cast<double>(res) * (res - 1)));
}

class TerrainGenerator : public ::testing::Test {
protected:
    [[nodiscard]] static TerrainParams params() {
        TerrainParams tp;
        tp.resolution = 257;
        tp.erosion_droplets = 20'000;
        return tp;
    }
    // The eroded reference map, generated once for the suite.
    [[nodiscard]] static const Heightmap& reference() {
        static const Heightmap map = generate(params());
        return map;
    }
};

TEST_F(TerrainGenerator, ProducesAValidMapWithPlausibleHeights) {
    const Heightmap& m = reference();
    ASSERT_TRUE(m.valid());
    const float height_m = params().height_m;
    EXPECT_GE(m.min_height, -1.0F);
    EXPECT_LE(m.max_height, height_m + 1.0F);
    EXPECT_GE(m.max_height - m.min_height, 0.05F * height_m) << "implausibly flat";
}

TEST_F(TerrainGenerator, SameParamsReproduceTheExactBits) {
    EXPECT_TRUE(bit_identical(generate(params()), reference()));
}

TEST_F(TerrainGenerator, DifferentSeedChangesTheMap) {
    TerrainParams other = params();
    other.seed += 1;
    EXPECT_FALSE(bit_identical(generate(other), reference()));
}

TEST_F(TerrainGenerator, DomainWarpChangesTheField) {
    TerrainParams unwarped = params();
    unwarped.warp_strength = 0.0F;
    unwarped.erosion_droplets = 0;
    TerrainParams warped = unwarped;
    warped.warp_strength = params().warp_strength;
    ASSERT_GT(warped.warp_strength, 0.0F);
    EXPECT_FALSE(bit_identical(generate(unwarped), generate(warped)));
}

// Erosion moves material: the mean local gradient drops vs the uneroded field.
TEST_F(TerrainGenerator, ErosionSmoothsTheField) {
    TerrainParams uneroded = params();
    uneroded.erosion_droplets = 0;
    EXPECT_LT(mean_abs_gradient(reference()), mean_abs_gradient(generate(uneroded)));
}

TEST_F(TerrainGenerator, NormalAtTheTileCentreIsUnitLengthAndUpward) {
    const Heightmap& m = reference();
    const float centre = m.tile_size_m * 0.5F;
    const glm::vec3 n = m.normal(centre, centre);
    EXPECT_GT(n.y, 0.0F);
    EXPECT_NEAR(glm::length(n), 1.0F, 1e-3F);
}

} // namespace
