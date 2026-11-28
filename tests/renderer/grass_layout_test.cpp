// GPU grass CPU helpers (R9.3): candidate grid, distance LOD, capacities.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/grass.hpp"

namespace {

namespace r = engine::renderer;

TEST(GrassLayout, GridCoversTheDiscInWholeTiles) {
    r::GrassSettings s;
    s.density = 25.0F; // 0.2 m cells
    s.radius = 10.0F;
    const glm::vec3 cam(3.05F, 1.0F, -7.31F);
    const r::GrassGrid g = r::grass_grid(s, cam);
    EXPECT_FLOAT_EQ(g.cell_size, 0.2F);
    EXPECT_EQ(g.cells.x % r::grass_tile_cells, 0U);
    EXPECT_EQ(g.cells, g.groups * r::grass_tile_cells);
    // Cells [origin, origin + cells) span at least [cam - radius, cam + radius].
    const glm::vec2 lo = glm::vec2(g.origin) * g.cell_size;
    const glm::vec2 hi = (glm::vec2(g.origin) + glm::vec2(g.cells)) * g.cell_size;
    EXPECT_LE(lo.x, cam.x - s.radius);
    EXPECT_LE(lo.y, cam.z - s.radius);
    EXPECT_GE(hi.x, cam.x + s.radius);
    EXPECT_GE(hi.y, cam.z + s.radius);
    // The grid is aligned to world cells: a camera move inside a cell keeps it.
    EXPECT_EQ(r::grass_grid(s, cam + glm::vec3(0.01F, 5.0F, 0.01F)).origin, g.origin);
    EXPECT_EQ(r::grass_grid(s, cam + glm::vec3(0.2F, 0.0F, 0.0F)).origin, g.origin + glm::ivec2(1, 0));
}

TEST(GrassLayout, OversizedFieldsCoarsenTheGrid) {
    r::GrassSettings s;
    s.density = 1000.0F;
    s.radius = 500.0F;
    const r::GrassGrid g = r::grass_grid(s, glm::vec3(0.0F));
    EXPECT_LE(static_cast<std::uint64_t>(2 * std::ceil(s.radius / g.cell_size) + 1) *
                  static_cast<std::uint64_t>(2 * std::ceil(s.radius / g.cell_size) + 1),
              r::grass_max_cells);
    EXPECT_GT(g.cell_size, 1.0F / std::sqrt(s.density));

    s.density = 0.0F;
    EXPECT_EQ(r::grass_cell_size(s), 0.0F);
    EXPECT_EQ(r::grass_capacity(s).near + r::grass_capacity(s).far, 0U);
}

TEST(GrassLayout, KeepFractionFallsSmoothlyWithDistance) {
    r::GrassSettings s;
    s.radius = 40.0F;
    s.lod_start = 0.25F;
    s.far_density = 0.2F;
    EXPECT_FLOAT_EQ(r::grass_keep_fraction(s, 0.0F), 1.0F);
    EXPECT_FLOAT_EQ(r::grass_keep_fraction(s, 10.0F), 1.0F);
    EXPECT_EQ(r::grass_keep_fraction(s, 40.0F), 0.0F);
    EXPECT_EQ(r::grass_keep_fraction(s, 50.0F), 0.0F);
    // Up to the edge band only thinning applies; it reaches far_density at the radius.
    const float band = 40.0F * (1.0F - r::grass_edge_band);
    EXPECT_FLOAT_EQ(r::grass_keep_fraction(s, band), r::grass_thinning(s, band));
    EXPECT_LT(r::grass_thinning(s, band), 0.3F);
    float prev = 1.0F;
    for (float d = 0.0F; d <= 40.0F; d += 0.1F) {
        const float k = r::grass_keep_fraction(s, d);
        EXPECT_LE(k, prev + 1e-6F);
        EXPECT_LE(prev - k, 0.02F) << "keep fraction jumps at " << d; // no popping
        prev = k;
    }
    EXPECT_NEAR(r::grass_thinning(s, 40.0F), 0.2F, 1e-6F);
}

TEST(GrassLayout, BladesShrinkBeforeTheyDrop) {
    // Full density: every threshold is a full-height blade.
    for (float t = 0.0F; t < 1.0F; t += 0.01F) EXPECT_FLOAT_EQ(r::grass_blade_fade(1.0F, t), 1.0F);
    // As the kept fraction falls past a blade's threshold, its height goes
    // continuously to zero.
    const float threshold = 0.5F;
    float prev = 1.0F;
    for (float keep = 1.0F; keep >= 0.0F; keep -= 0.005F) {
        const float f = r::grass_blade_fade(keep, threshold);
        EXPECT_LE(f, prev + 1e-6F);
        EXPECT_LE(prev - f, 0.05F);
        prev = f;
    }
    EXPECT_EQ(prev, 0.0F);
}

TEST(GrassLayout, CapacityBoundsEveryCellThatCanHoldABlade) {
    r::GrassSettings s;
    s.density = 16.0F; // 0.25 m cells
    s.radius = 12.0F;
    s.lod_start = 0.4F;
    const r::GrassCapacity cap = r::grass_capacity(s);
    const glm::vec3 cam(0.13F, 0.0F, 0.71F);
    const r::GrassGrid g = r::grass_grid(s, cam);
    std::uint32_t near = 0;
    std::uint32_t all = 0;
    for (std::uint32_t z = 0; z < g.cells.y; ++z) {
        for (std::uint32_t x = 0; x < g.cells.x; ++x) {
            const auto c = r::grass_candidate(s, g.cell_size, g.origin + glm::ivec2(x, z));
            const float d = glm::length(c.root_xz - glm::vec2(cam.x, cam.z));
            if (d < s.radius) ++all;
            if (d < s.lod_start * s.radius) ++near;
        }
    }
    EXPECT_GE(cap.near, near);
    EXPECT_GE(cap.far, all);
    EXPECT_LE(cap.near + cap.far, r::grass_max_blades);
}

TEST(GrassLayout, CandidatesAreDeterministicPerSeed) {
    r::GrassSettings s;
    const auto a = r::grass_candidate(s, 0.5F, {-3, 7});
    const auto b = r::grass_candidate(s, 0.5F, {-3, 7});
    EXPECT_EQ(a.root_xz, b.root_xz);
    EXPECT_EQ(a.threshold, b.threshold);
    EXPECT_EQ(a.hash, b.hash);
    // Inside its own cell.
    EXPECT_GE(a.root_xz.x, -1.5F);
    EXPECT_LT(a.root_xz.x, -1.0F);
    EXPECT_GE(a.root_xz.y, 3.5F);
    EXPECT_LT(a.root_xz.y, 4.0F);
    s.seed = 2;
    EXPECT_NE(r::grass_candidate(s, 0.5F, {-3, 7}).root_xz, a.root_xz);
}

TEST(GrassLayout, SanitizeClampsAndNormalises) {
    r::GrassSettings s;
    s.wind_direction = glm::vec2(3.0F, 4.0F);
    s.height_range = glm::vec2(2.0F, 0.5F);
    s.slope_deg = glm::vec2(-5.0F, 120.0F);
    s.density = -1.0F;
    s.lod_start = 3.0F;
    const r::GrassSettings c = r::sanitize(s);
    EXPECT_NEAR(c.wind_direction.x, 0.6F, 1e-6F);
    EXPECT_NEAR(c.wind_direction.y, 0.8F, 1e-6F);
    EXPECT_EQ(c.height_range, glm::vec2(0.5F, 2.0F));
    EXPECT_EQ(c.slope_deg, glm::vec2(0.0F, 90.0F));
    EXPECT_EQ(c.density, 0.0F);
    EXPECT_EQ(c.lod_start, 1.0F);
    s.wind_direction = glm::vec2(0.0F);
    EXPECT_EQ(r::sanitize(s).wind_direction, glm::vec2(1.0F, 0.0F));
}

// The slope fade: full density up to grass_slope_fade_deg below the
// steepest allowed slope, then thinning smoothly to 0 at it.
TEST(GrassLayout, SlopeFadeThinsBelowTheSteepestSlope) {
    const auto ny = [](float deg) { return std::cos(glm::radians(deg)); };
    r::GrassSettings s;
    s.slope_deg = glm::vec2(0.0F, 40.0F);
    EXPECT_NEAR(r::grass_slope_fade_start(s), ny(40.0F - r::grass_slope_fade_deg), 1e-6F);
    EXPECT_EQ(r::grass_slope_keep(s, 1.0F), 1.0F);
    EXPECT_EQ(r::grass_slope_keep(s, ny(30.0F)), 1.0F);
    EXPECT_EQ(r::grass_slope_keep(s, ny(32.0F) + 1e-6F), 1.0F);
    EXPECT_NEAR(r::grass_slope_keep(s, ny(40.0F)), 0.0F, 1e-6F);
    float previous = 1.0F;
    for (float deg = 32.5F; deg < 40.0F; deg += 0.5F) {
        const float keep = r::grass_slope_keep(s, ny(deg));
        EXPECT_GT(keep, 0.0F) << deg;
        EXPECT_LT(keep, previous) << deg; // strictly thinning toward the limit
        previous = keep;
    }
    // The band never reaches below the shallowest allowed slope.
    s.slope_deg = glm::vec2(36.0F, 40.0F);
    EXPECT_NEAR(r::grass_slope_fade_start(s), ny(36.0F), 1e-6F);
    // No upper limit: no fade.
    s.slope_deg = glm::vec2(0.0F, 90.0F);
    EXPECT_EQ(r::grass_slope_fade_start(s), -2.0F);
    EXPECT_EQ(r::grass_slope_keep(s, ny(80.0F)), 1.0F);
    // A zero-width range keeps the hard edge.
    s.slope_deg = glm::vec2(30.0F, 30.0F);
    EXPECT_EQ(r::grass_slope_keep(s, ny(29.0F)), 1.0F);
    EXPECT_EQ(r::grass_slope_keep(s, ny(31.0F)), 0.0F);
}

TEST(GrassLayout, StripIndexCounts) {
    EXPECT_EQ(r::grass_index_count(r::grass_near_segments), 39U);
    EXPECT_EQ(r::grass_index_count(r::grass_far_segments), 15U);
}

TEST(GrassLayout, FrustumPlanesAreNormalisedAndInward) {
    glm::mat4 proj = glm::perspective(glm::radians(60.0F), 1.0F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F;
    const glm::mat4 vp = proj * glm::lookAt(glm::vec3(0.0F), glm::vec3(0.0F, 0.0F, -1.0F),
                                            glm::vec3(0.0F, 1.0F, 0.0F));
    for (const glm::vec4& p : r::grass_frustum_planes(vp)) {
        EXPECT_NEAR(glm::length(glm::vec3(p)), 1.0F, 1e-5F);
        EXPECT_GT(glm::dot(glm::vec3(p), glm::vec3(0.0F, 0.0F, -10.0F)) + p.w, 0.0F);
    }
}

} // namespace

// Grass v2 (#119): every root lies `clump_strength` of the way to the centre
// of its clump; clumps are fixed in the world and the pull never exceeds its
// bound.
TEST(GrassLayout, ClumpsPullRootsTowardTheNearestCentre) {
    r::GrassSettings s;
    s.seed = 9;
    s.clump_size = 0.6F;
    s.clump_strength = 0.4F;
    r::GrassSettings loose = s;
    loose.clump_size = 0.0F;
    const float cell = 0.1F;
    std::uint32_t distinct_clumps = 0;
    std::uint32_t last = 0;
    for (int z = -40; z < 40; z += 3) {
        for (int x = -40; x < 40; x += 3) {
            const glm::ivec2 c(x, z);
            const glm::vec2 own = r::grass_candidate(loose, cell, c).root_xz;
            const r::GrassCandidate pulled = r::grass_candidate(s, cell, c);
            const r::GrassClump clump = r::grass_clump(s, own);
            EXPECT_EQ(pulled.clump_hash, clump.hash);
            EXPECT_NEAR(glm::length(pulled.root_xz - own),
                        glm::length(clump.centre - own) * s.clump_strength, 1e-5F);
            EXPECT_LE(glm::length(pulled.root_xz - own), r::grass_max_clump_pull(s) + 1e-6F);
            if (clump.hash != last) ++distinct_clumps;
            last = clump.hash;
            // Same thing, same answer (fixed in the world).
            EXPECT_EQ(r::grass_candidate(s, cell, c).root_xz, pulled.root_xz);
        }
    }
    EXPECT_GT(distinct_clumps, 100U);
    // No clumps: the root is the jittered cell position and no clump hash.
    EXPECT_EQ(r::grass_candidate(loose, cell, {3, 4}).clump_hash, 0U);
    EXPECT_FLOAT_EQ(r::grass_max_clump_pull(loose), 0.0F);
}

// The pixel-footprint term keeps every blade while the mean blade spans at
// least min_blade_px, then falls as 1 / distance to its floor; survivors widen
// by the inverse of the kept share so the projected coverage holds.
TEST(GrassLayout, FootprintKeepHoldsProjectedCoverage) {
    r::GrassSettings s;
    s.width_range = {0.01F, 0.03F}; // mean 2 cm
    s.min_blade_px = 2.0F;
    const float ppm = 500.0F; // 2 cm spans 2 px at 5 m
    EXPECT_FLOAT_EQ(r::grass_footprint_keep(s, 1.0F, ppm), 1.0F);
    EXPECT_FLOAT_EQ(r::grass_footprint_keep(s, 5.0F, ppm), 1.0F);
    EXPECT_NEAR(r::grass_footprint_keep(s, 10.0F, ppm), 0.5F, 1e-6F);
    EXPECT_NEAR(r::grass_footprint_keep(s, 20.0F, ppm), 0.25F, 1e-6F);
    EXPECT_FLOAT_EQ(r::grass_footprint_keep(s, 1.0e5F, ppm), r::grass_min_footprint_keep);
    EXPECT_FLOAT_EQ(r::grass_footprint_keep(s, 20.0F, 0.0F), 1.0F); // off (orthographic)
    s.min_blade_px = 0.0F;
    EXPECT_FLOAT_EQ(r::grass_footprint_keep(s, 20.0F, ppm), 1.0F); // off (setting)
    // Coverage = kept share x width scale stays 1 until the width cap.
    for (const float keep : {1.0F, 0.8F, 0.5F, 0.3F}) {
        EXPECT_NEAR(keep * r::grass_width_scale(keep), 1.0F, 1e-6F);
    }
    EXPECT_FLOAT_EQ(r::grass_width_scale(0.01F), r::grass_max_width_scale);
    // Past the cap (kept share below 1 / max scale) coverage falls with the
    // kept share; the far ground cover carries the colour from there.
    const float floor = 1.0F / r::grass_max_width_scale;
    for (const float keep : {floor * 0.8F, floor * 0.5F, 0.0125F}) {
        EXPECT_NEAR(keep * r::grass_width_scale(keep), keep / floor, 1e-6F);
        EXPECT_LT(keep * r::grass_width_scale(keep), 1.0F);
    }
}

// The ground cover's outer bound (#143 review): how far the bound height
// tiles reach around the camera, and the fade derived from it.
TEST(GrassLayout, TileCoverageBoundsTheGroundCover) {
    // One 64 m tile with the camera 10 m from its west edge.
    const std::array<glm::vec3, 1> one{glm::vec3(-10.0F, -32.0F, 64.0F)};
    EXPECT_NEAR(r::grass_tile_coverage(one, {0.0F, 0.0F}, 100.0F), 10.0F, 0.5F);
    // Capped at the requested distance; outside every tile: nothing.
    EXPECT_FLOAT_EQ(r::grass_tile_coverage(one, {20.0F, 0.0F}, 5.0F), 5.0F);
    EXPECT_FLOAT_EQ(r::grass_tile_coverage(one, {-50.0F, 0.0F}, 100.0F), 0.0F);
    // Two adjacent tiles cover their union.
    const std::array<glm::vec3, 2> two{glm::vec3(-64.0F, -32.0F, 64.0F),
                                       glm::vec3(0.0F, -32.0F, 64.0F)};
    EXPECT_NEAR(r::grass_tile_coverage(two, {0.0F, 0.0F}, 100.0F), 32.0F, 0.5F);

    r::GrassSettings s;
    s.radius = 20.0F;
    // Flat ground without tiles: unbounded.
    EXPECT_TRUE(std::isinf(r::grass_cover_fade(s, 0.0F, false).y));
    // Tiles reaching far: fade from the radius to 1.5 x radius.
    EXPECT_EQ(r::grass_cover_fade(s, 500.0F, true), glm::vec2(20.0F, 30.0F));
    // Tiles reaching less than the radius: the fade ends at their edge.
    const glm::vec2 near_edge = r::grass_cover_fade(s, 12.0F, true);
    EXPECT_FLOAT_EQ(near_edge.y, 12.0F);
    EXPECT_FLOAT_EQ(near_edge.x, 8.0F);
}

TEST(GrassLayout, SanitizeClampsTheV2Fields) {
    r::GrassSettings s;
    s.clump_size = -1.0F;
    s.clump_strength = 3.0F;
    s.color_variation = std::numeric_limits<float>::quiet_NaN();
    s.dry_color = glm::vec3(2.0F, -1.0F, 0.5F);
    s.ground_cover = 1.5F;
    s.min_blade_px = 100.0F;
    const r::GrassSettings c = r::sanitize(s);
    EXPECT_EQ(c.clump_size, 0.0F);
    EXPECT_EQ(c.clump_strength, 0.9F);
    EXPECT_EQ(c.color_variation, 0.0F);
    EXPECT_EQ(c.dry_color, glm::vec3(1.0F, 0.0F, 0.5F));
    EXPECT_EQ(c.ground_cover, 1.0F);
    EXPECT_EQ(c.min_blade_px, 8.0F);
}
