// GPU grass CPU helpers (R9.3): candidate grid, distance LOD, capacities.

#include <cmath>
#include <cstdint>

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
