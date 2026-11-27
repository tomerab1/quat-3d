// Procedural scattering (R9.2): determinism, density / spacing statistics,
// ground filtering, entity frames and regeneration.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <optional>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <gtest/gtest.h>

#include "engine/scene/components.hpp"
#include "engine/scene/instanced_mesh.hpp"
#include "engine/scene/scatter.hpp"
#include "engine/terrain/heightmap.hpp"

namespace {

namespace s = engine::scene;
using engine::terrain::Heightmap;

// A square heightmap of `size` metres at 1 m per texel, heights from `h`
// (local metres), placed with its minimum corner at `origin`.
Heightmap make_map(float size, const std::function<float(float x, float z)>& h) {
    Heightmap map;
    map.resolution = static_cast<std::uint32_t>(size) + 1;
    map.tile_size_m = size;
    map.heights.resize(static_cast<std::size_t>(map.resolution) * map.resolution);
    map.min_height = 1e30F;
    map.max_height = -1e30F;
    for (std::uint32_t z = 0; z < map.resolution; ++z) {
        for (std::uint32_t x = 0; x < map.resolution; ++x) {
            const float v = h(static_cast<float>(x), static_cast<float>(z));
            map.heights[static_cast<std::size_t>(z) * map.resolution + x] = v;
            map.min_height = std::min(map.min_height, v);
            map.max_height = std::max(map.max_height, v);
        }
    }
    return map;
}

float min_pair_distance(const std::vector<glm::vec2>& pts) {
    float best = 1e30F;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        for (std::size_t j = i + 1; j < pts.size(); ++j) {
            best = std::min(best, glm::distance(pts[i], pts[j]));
        }
    }
    return best;
}

bool same(const s::InstanceTransform& a, const s::InstanceTransform& b) {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
}

s::Scatter rect_scatter(float w, float d, float density, float spacing) {
    s::Scatter sc;
    sc.area = s::ScatterArea::rect;
    sc.size = {w, d};
    sc.density = density;
    sc.min_spacing = spacing;
    sc.slope_deg = {0.0F, 90.0F};
    return sc;
}

const glm::mat4 k_identity(1.0F);

TEST(Scatter, SameSeedSamePlacementsOtherSeedDifferent) {
    s::Scatter sc = rect_scatter(60.0F, 60.0F, 0.08F, 1.5F);
    sc.seed = 77;
    const auto a = s::generate_scatter(sc, k_identity, {});
    const auto b = s::generate_scatter(sc, k_identity, {});
    ASSERT_FALSE(a.empty());
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) EXPECT_TRUE(same(a[i], b[i])) << i;

    sc.seed = 78;
    const auto c = s::generate_scatter(sc, k_identity, {});
    std::size_t identical = 0;
    for (std::size_t i = 0; i < std::min(a.size(), c.size()); ++i) identical += same(a[i], c[i]);
    EXPECT_LT(identical, a.size() / 10);

    sc.seed = 77;
    sc.pattern = s::ScatterPattern::jittered_grid;
    const auto g1 = s::generate_scatter(sc, k_identity, {});
    const auto g2 = s::generate_scatter(sc, k_identity, {});
    ASSERT_EQ(g1.size(), g2.size());
    for (std::size_t i = 0; i < g1.size(); ++i) EXPECT_TRUE(same(g1[i], g2[i])) << i;
}

TEST(Scatter, PoissonHitsTargetDensityUniformlyWithSpacing) {
    const s::Scatter sc = rect_scatter(100.0F, 100.0F, 0.05F, 2.0F);
    const auto pts = s::scatter_candidates(sc);
    EXPECT_EQ(pts.size(), 500U); // round(0.05 * 100 * 100)
    EXPECT_GE(min_pair_distance(pts), 2.0F);

    // Spread evenly: each 50x50 quadrant holds a quarter (+-25%).
    int quadrant[4] = {0, 0, 0, 0};
    for (const glm::vec2& p : pts) {
        EXPECT_LE(std::abs(p.x), 50.0F);
        EXPECT_LE(std::abs(p.y), 50.0F);
        ++quadrant[(p.x < 0.0F ? 0 : 1) + (p.y < 0.0F ? 0 : 2)];
    }
    for (const int q : quadrant) {
        EXPECT_GT(q, 94);
        EXPECT_LT(q, 156);
    }
}

TEST(Scatter, SpacingCapsDensity) {
    // 1 / m^2 cannot fit 1.5 m spacing: Bridson saturates near 0.65 / r^2.
    const s::Scatter sc = rect_scatter(40.0F, 40.0F, 1.0F, 1.5F);
    const auto pts = s::scatter_candidates(sc);
    EXPECT_LT(pts.size(), 1600U);
    EXPECT_GT(pts.size(), static_cast<std::size_t>(0.5F / 2.25F * 1600.0F));
    EXPECT_GE(min_pair_distance(pts), 1.5F - 1e-4F);
}

TEST(Scatter, JitteredGridKeepsSpacingAndCount) {
    s::Scatter sc = rect_scatter(40.0F, 40.0F, 0.25F, 1.0F); // 2 m cells
    sc.pattern = s::ScatterPattern::jittered_grid;
    const auto pts = s::scatter_candidates(sc);
    EXPECT_EQ(pts.size(), 400U);
    EXPECT_GE(min_pair_distance(pts), 1.0F - 1e-4F);
    // One per cell.
    std::vector<int> cells(400, 0);
    for (const glm::vec2& p : pts) {
        const int cx = static_cast<int>((p.x + 20.0F) / 2.0F);
        const int cz = static_cast<int>((p.y + 20.0F) / 2.0F);
        ASSERT_GE(cx, 0);
        ASSERT_LT(cx, 20);
        ASSERT_GE(cz, 0);
        ASSERT_LT(cz, 20);
        ++cells[static_cast<std::size_t>(cz * 20 + cx)];
    }
    EXPECT_TRUE(std::all_of(cells.begin(), cells.end(), [](int c) { return c == 1; }));
}

TEST(Scatter, CircleAreaAndInstanceCap) {
    s::Scatter sc = rect_scatter(0.0F, 0.0F, 0.02F, 1.0F);
    sc.area = s::ScatterArea::circle;
    sc.radius = 30.0F;
    const auto pts = s::scatter_candidates(sc);
    EXPECT_EQ(pts.size(), static_cast<std::size_t>(std::lround(0.02 * std::numbers::pi * 900.0)));
    for (const glm::vec2& p : pts) EXPECT_LE(glm::length(p), 30.0F + 1e-4F);

    s::Scatter capped = rect_scatter(100.0F, 100.0F, 1.0F, 0.0F);
    capped.max_instances = 300;
    const auto few = s::scatter_candidates(capped);
    EXPECT_EQ(few.size(), 300U);
    // Capping enlarges the spacing instead of filling one corner.
    int left = 0;
    for (const glm::vec2& p : few) left += p.x < 0.0F;
    EXPECT_GT(left, 100);
    EXPECT_LT(left, 200);

    s::Scatter none = rect_scatter(100.0F, 100.0F, 0.0F, 1.0F);
    EXPECT_TRUE(s::scatter_candidates(none).empty());
}

TEST(Scatter, ScaleAndYawRanges) {
    s::Scatter sc = rect_scatter(50.0F, 50.0F, 0.1F, 1.0F);
    sc.scale_range = {0.5F, 2.0F};
    sc.random_yaw = false;
    const auto upright = s::generate_scatter(sc, k_identity, {});
    ASSERT_FALSE(upright.empty());
    float lo = 1e30F;
    float hi = -1e30F;
    for (const auto& t : upright) {
        EXPECT_FLOAT_EQ(t.scale.x, t.scale.y);
        EXPECT_FLOAT_EQ(t.scale.x, t.scale.z);
        lo = std::min(lo, t.scale.x);
        hi = std::max(hi, t.scale.x);
        EXPECT_NEAR(std::abs(t.rotation.w), 1.0F, 1e-6F);
    }
    EXPECT_GE(lo, 0.5F);
    EXPECT_LE(hi, 2.0F);
    EXPECT_LT(lo, 0.7F); // the range is actually used
    EXPECT_GT(hi, 1.8F);

    sc.random_yaw = true;
    const auto turned = s::generate_scatter(sc, k_identity, {});
    ASSERT_EQ(turned.size(), upright.size());
    float max_angle = 0.0F;
    for (const auto& t : turned) {
        const glm::vec3 up = t.rotation * glm::vec3(0.0F, 1.0F, 0.0F);
        EXPECT_NEAR(up.y, 1.0F, 1e-5F); // yaw only
        max_angle = std::max(max_angle, glm::angle(t.rotation));
    }
    EXPECT_GT(max_angle, 2.5F);
}

TEST(Scatter, SlopeAndHeightLimitsOnSyntheticTerrain) {
    // Tile [-50, 50]^2: flat at 0 for world x < 0, a 45 degree ramp beyond.
    const Heightmap map =
        make_map(100.0F, [](float x, float) { return x < 50.0F ? 0.0F : x - 50.0F; });
    const s::ScatterGround ground = s::make_tile_ground({{&map, glm::vec3(-50.0F, 0.0F, -50.0F)}});

    s::Scatter sc = rect_scatter(96.0F, 96.0F, 0.1F, 1.0F);
    sc.slope_deg = {0.0F, 20.0F};
    const auto flat = s::generate_scatter(sc, k_identity, ground);
    ASSERT_GT(flat.size(), 300U);
    for (const auto& t : flat) {
        EXPECT_LT(t.position.x, 1.5F); // the ramp is 45 degrees
        const auto g = ground.sample(t.position.x, t.position.z);
        ASSERT_TRUE(g.has_value());
        EXPECT_FLOAT_EQ(t.position.y, g->height);
        EXPECT_LE(glm::degrees(std::acos(glm::normalize(g->normal).y)), 20.0F + 1e-3F);
    }

    sc.slope_deg = {30.0F, 60.0F};
    const auto steep = s::generate_scatter(sc, k_identity, ground);
    ASSERT_GT(steep.size(), 300U);
    for (const auto& t : steep) EXPECT_GT(t.position.x, -1.5F);

    // Height window [5, 20] m: only part of the ramp qualifies.
    sc.slope_deg = {0.0F, 90.0F};
    sc.height_m = {5.0F, 20.0F};
    const auto band = s::generate_scatter(sc, k_identity, ground);
    ASSERT_GT(band.size(), 50U);
    for (const auto& t : band) {
        EXPECT_GE(t.position.y, 5.0F);
        EXPECT_LE(t.position.y, 20.0F);
        EXPECT_GT(t.position.x, 4.0F);
        EXPECT_LT(t.position.x, 21.0F);
    }
}

TEST(Scatter, AlignToNormalTiltsPlacements) {
    const float slope = glm::radians(30.0F);
    const Heightmap map = make_map(100.0F, [&](float x, float) { return x * std::tan(slope); });
    const s::ScatterGround ground = s::make_tile_ground({{&map, glm::vec3(-50.0F, 0.0F, -50.0F)}});
    const glm::vec3 normal = glm::normalize(glm::vec3(-std::sin(slope), std::cos(slope), 0.0F));

    s::Scatter sc = rect_scatter(80.0F, 80.0F, 0.05F, 1.0F);
    sc.random_yaw = true;
    for (const float align : {0.0F, 0.5F, 1.0F}) {
        sc.align_to_normal = align;
        const auto placed = s::generate_scatter(sc, k_identity, ground);
        ASSERT_FALSE(placed.empty());
        for (const auto& t : placed) {
            const glm::vec3 up = t.rotation * glm::vec3(0.0F, 1.0F, 0.0F);
            const float tilt = glm::degrees(std::acos(glm::clamp(up.y, -1.0F, 1.0F)));
            EXPECT_NEAR(tilt, 30.0F * align, 0.05F) << "align " << align;
            if (align == 1.0F) EXPECT_NEAR(glm::dot(up, normal), 1.0F, 1e-4F);
        }
    }

    // base_offset lifts along the tilted up axis, scaled by the instance scale.
    sc.align_to_normal = 1.0F;
    sc.base_offset = 0.5F;
    for (const auto& t : s::generate_scatter(sc, k_identity, ground)) {
        const auto g = ground.sample(t.position.x - normal.x * 0.5F * t.scale.x,
                                     t.position.z - normal.z * 0.5F * t.scale.x);
        ASSERT_TRUE(g.has_value());
        EXPECT_NEAR(t.position.y, g->height + normal.y * 0.5F * t.scale.x, 1e-3F);
    }
}

TEST(Scatter, PartialGroundKeepsOtherPlacementsUnchanged) {
    // Streaming: a tile that is not loaded yet must not reshuffle the rest.
    const Heightmap map = make_map(100.0F, [](float x, float z) { return 0.05F * (x + z); });
    const s::ScatterGround full = s::make_tile_ground({{&map, glm::vec3(-50.0F, 0.0F, -50.0F)}});
    s::ScatterGround west;
    west.sample = [&](float x, float z) -> std::optional<s::GroundSample> {
        if (x >= 0.0F) return std::nullopt;
        return full.sample(x, z);
    };
    const s::Scatter sc = rect_scatter(90.0F, 90.0F, 0.1F, 1.0F);
    const auto all = s::generate_scatter(sc, k_identity, full);
    const auto part = s::generate_scatter(sc, k_identity, west);
    std::vector<s::InstanceTransform> expected;
    for (const auto& t : all) {
        if (t.position.x < 0.0F) expected.push_back(t);
    }
    ASSERT_EQ(part.size(), expected.size());
    for (std::size_t i = 0; i < part.size(); ++i) EXPECT_TRUE(same(part[i], expected[i])) << i;
}

TEST(Scatter, PlacementsAreRelativeToTheEntity) {
    // Flat ground (no terrain) at the entity's height; rotated + scaled
    // entity: world placements stay upright, on the plane, inside the area.
    const glm::mat4 world = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 5.0F, -3.0F)) *
                            glm::rotate(glm::mat4(1.0F), glm::radians(90.0F), glm::vec3(0, 1, 0)) *
                            glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    s::Scatter sc = rect_scatter(20.0F, 10.0F, 0.2F, 0.5F);
    sc.scale_range = {1.0F, 1.0F};
    const auto placed = s::generate_scatter(sc, world, {});
    ASSERT_EQ(placed.size(), 40U);
    for (const auto& t : placed) {
        const glm::mat4 m = world * s::instance_matrix(t);
        const glm::vec3 p(m[3]);
        EXPECT_NEAR(p.y, 5.0F, 1e-4F);
        EXPECT_LE(std::abs(p.x - 10.0F), 10.0F + 1e-3F); // area axis-aligned in world XZ
        EXPECT_LE(std::abs(p.z + 3.0F), 5.0F + 1e-3F);
        // World scale 1 (the entity's 2x is folded out), world up = +Y.
        EXPECT_NEAR(glm::length(glm::vec3(m[1])), 1.0F, 1e-4F);
        EXPECT_NEAR(glm::normalize(glm::vec3(m[1])).y, 1.0F, 1e-4F);
    }
}

TEST(Scatter, TileGroundSamplesAndRevisions) {
    Heightmap map = make_map(10.0F, [](float, float) { return 2.0F; });
    const glm::vec3 origin(100.0F, 1.0F, 100.0F);
    const s::ScatterGround ground = s::make_tile_ground({{&map, origin}});
    ASSERT_TRUE(ground.sample(105.0F, 105.0F).has_value());
    EXPECT_FLOAT_EQ(ground.sample(105.0F, 105.0F)->height, 3.0F);
    EXPECT_FALSE(ground.sample(95.0F, 105.0F).has_value());

    const std::uint64_t over = ground.revision({104.0F, 104.0F}, {106.0F, 106.0F});
    const std::uint64_t away = ground.revision({0.0F, 0.0F}, {10.0F, 10.0F});
    map.heights[map.heights.size() / 2] = 4.0F; // regenerated tile, same range
    map.max_height = 4.0F;
    EXPECT_NE(ground.revision({104.0F, 104.0F}, {106.0F, 106.0F}), over);
    EXPECT_EQ(ground.revision({0.0F, 0.0F}, {10.0F, 10.0F}), away);
}

TEST(Scatter, SystemRegeneratesOnlyOnChange) {
    entt::registry r;
    const entt::entity e = r.create();
    r.emplace<s::Transform>(e).world =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, 0.0F));
    r.emplace<s::Scatter>(e, rect_scatter(30.0F, 30.0F, 0.1F, 1.0F));

    std::uint64_t revision = 1;
    s::ScatterGround ground;
    ground.revision = [&](glm::vec2, glm::vec2) { return revision; };

    EXPECT_EQ(s::scatter_system(r, ground), 1U);
    ASSERT_TRUE(r.all_of<s::InstancedMesh>(e)); // added on demand
    const std::size_t count = r.get<s::InstancedMesh>(e).instances.size();
    EXPECT_EQ(count, 90U);
    EXPECT_EQ(r.get<s::Scatter>(e).generation, 1U);

    EXPECT_EQ(s::scatter_system(r, ground), 0U); // nothing changed
    EXPECT_EQ(r.get<s::Scatter>(e).generation, 1U);

    r.get<s::Scatter>(e).density = 0.2F; // parameters
    EXPECT_EQ(s::scatter_system(r, ground), 1U);
    EXPECT_EQ(r.get<s::InstancedMesh>(e).instances.size(), 180U);

    revision = 2; // the terrain under the area
    EXPECT_EQ(s::scatter_system(r, ground), 1U);

    r.get<s::Transform>(e).world = glm::translate(glm::mat4(1.0F), glm::vec3(1.0F, 0.0F, 0.0F));
    EXPECT_EQ(s::scatter_system(r, ground), 1U); // the entity moved
    EXPECT_EQ(r.get<s::Scatter>(e).generation, 4U);

    // Authored mesh settings survive regeneration.
    r.get<s::InstancedMesh>(e).primitive.shape = s::PrimitiveShape::cone;
    r.get<s::Scatter>(e).seed = 9;
    EXPECT_EQ(s::scatter_system(r, ground), 1U);
    EXPECT_EQ(r.get<s::InstancedMesh>(e).primitive.shape, s::PrimitiveShape::cone);
}

} // namespace
