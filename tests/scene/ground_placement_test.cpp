// Ground placement (issue #7): a pedestal placed on generated terrain must
// neither float (its foot reaches the ground everywhere under it) nor be
// pierced (the ground never rises above its top), and its top sits at the
// sampled terrain height plus the clearance.

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/scene/ground_placement.hpp"
#include "engine/terrain/generator.hpp"

namespace {

using engine::scene::GroundHeightFn;
using engine::scene::GroundLattice;
using engine::scene::GroundSpan;

// A small eroded-free tile like the QUAT_TERRAIN demo (800 m, 90 m relief),
// anchored at `anchor` the way WorldRuntime samples it.
struct TerrainTile {
    engine::terrain::Heightmap map;
    glm::vec3                  anchor{0.0F, -40.0F, 0.0F};

    TerrainTile() {
        engine::terrain::TerrainParams p;
        p.resolution = 129;
        p.tile_size_m = 800.0F;
        p.height_m = 90.0F;
        p.erosion_droplets = 0;
        map = engine::terrain::generate(p);
    }

    // The texel lattice in world XZ (texel 0 at the tile's minimum corner).
    [[nodiscard]] GroundLattice lattice() const {
        const float half = map.tile_size_m * 0.5F;
        return {glm::vec2(anchor.x - half, anchor.z - half), map.metres_per_texel()};
    }

    [[nodiscard]] GroundHeightFn ground() const {
        return [this](float x, float z) -> std::optional<float> {
            const float half = map.tile_size_m * 0.5F;
            const float lx = x - (anchor.x - half);
            const float lz = z - (anchor.z - half);
            if (lx < 0.0F || lz < 0.0F || lx > map.tile_size_m || lz > map.tile_size_m) {
                return std::nullopt;
            }
            return anchor.y + map.sample(lx, lz);
        };
    }
};

TEST(GroundPlacement, PedestalOnTerrainNeitherFloatsNorIsPierced) {
    const TerrainTile tile;
    ASSERT_TRUE(tile.map.valid());
    const GroundHeightFn ground = tile.ground();
    const glm::vec2 centre(0.0F, 0.0F);
    const glm::vec2 half(14.0F, 14.0F); // the showcase plate

    const std::optional<GroundSpan> span =
        engine::scene::ground_span(ground, centre, half, tile.lattice());
    ASSERT_TRUE(span.has_value());
    EXPECT_LE(span->min_y, span->max_y);
    const auto pedestal = engine::scene::pedestal_on(*span, 0.8F, 0.5F);
    EXPECT_FLOAT_EQ(pedestal.top_y, span->max_y + 0.8F);
    EXPECT_FLOAT_EQ(pedestal.bottom_y, span->min_y - 0.5F);

    // Brute force on the texel lattice refined four times (plus the
    // footprint's edges): the ground stays between the pedestal's foot and
    // top everywhere under the footprint, and the top sits on the peak (+
    // clearance) — nothing pokes through, nothing floats.
    const GroundLattice lattice = tile.lattice();
    const auto axis = [&](float a, float b, float origin) {
        std::vector<float> v{a, b};
        const float step = lattice.spacing / 4.0F;
        for (float k = std::ceil((a - origin) / step); origin + k * step < b; k += 1.0F) {
            v.push_back(origin + k * step);
        }
        return v;
    };
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for (const float z : axis(centre.y - half.y, centre.y + half.y, lattice.origin.y)) {
        for (const float x : axis(centre.x - half.x, centre.x + half.x, lattice.origin.x)) {
            const float h = *ground(x, z);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
    EXPECT_NEAR(span->max_y, hi, 1e-3F) << "lattice sampling missed a peak";
    EXPECT_NEAR(span->min_y, lo, 1e-3F);
    EXPECT_GE(pedestal.top_y, hi);
    EXPECT_LE(pedestal.bottom_y, lo);
    EXPECT_GT(hi - lo, 0.1F) << "the test terrain should not be flat here";
}

TEST(GroundPlacement, FlatGroundGivesAThinPedestalAtTheGround) {
    const GroundHeightFn flat = [](float, float) -> std::optional<float> { return 3.0F; };
    const auto span = engine::scene::ground_span(flat, {5.0F, -2.0F}, {2.0F, 1.0F}, {{0.3F, 0.1F}, 0.5F});
    ASSERT_TRUE(span.has_value());
    EXPECT_FLOAT_EQ(span->min_y, 3.0F);
    EXPECT_FLOAT_EQ(span->max_y, 3.0F);
    const auto pedestal = engine::scene::pedestal_on(*span, 0.0F, 0.25F);
    EXPECT_FLOAT_EQ(pedestal.top_y, 3.0F);
    EXPECT_FLOAT_EQ(pedestal.depth(), 0.25F);
}

TEST(GroundPlacement, MissingGroundOrBadInputGivesNothing) {
    const TerrainTile tile;
    const GroundHeightFn ground = tile.ground();
    // Footprint hanging off the tile's edge.
    const GroundLattice lattice = tile.lattice();
    EXPECT_FALSE(engine::scene::ground_span(ground, {395.0F, 0.0F}, {14.0F, 14.0F}, lattice));
    EXPECT_FALSE(engine::scene::ground_span({}, {0.0F, 0.0F}, {1.0F, 1.0F}, lattice));
    EXPECT_FALSE(engine::scene::ground_span(ground, {0.0F, 0.0F}, {1.0F, 1.0F},
                                            {lattice.origin, 0.0F}));
    EXPECT_FALSE(engine::scene::ground_span(ground, {0.0F, 0.0F}, {-1.0F, 1.0F}, lattice));
}

} // namespace
