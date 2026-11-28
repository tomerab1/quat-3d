#include <cmath>

#include <gtest/gtest.h>

#include "engine/geometry/stone.hpp"
#include "geometry/geometry_test_util.hpp"
#include "support/expect_ok.hpp"

using namespace engine::geometry;
namespace asset = engine::asset;

TEST(GeometryStone, DeterministicPerSeed) {
    StoneOptions options;
    options.seed = 42;
    auto a = generate_stone(options);
    auto b = generate_stone(options);
    ASSERT_OK(a);
    ASSERT_OK(b);
    EXPECT_EQ(mesh_hash(*a), mesh_hash(*b));
    options.seed = 43;
    auto c = generate_stone(options);
    ASSERT_OK(c);
    EXPECT_NE(mesh_hash(*a), mesh_hash(*c));
}

TEST(GeometryStone, ClosedOutwardAndWithinSize) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        StoneOptions options;
        options.seed = seed;
        options.size = {0.5F, 0.3F, 0.4F};
        auto stone = generate_stone(options);
        ASSERT_OK(stone);
        ASSERT_OK(validate(*stone));
        EXPECT_GT(quat::test::geometry::signed_volume(*stone), 0.0) << seed;
        const asset::Aabb box = compute_bounds(*stone);
        const glm::vec3 limit = options.size * 0.5F + glm::vec3(options.noise_amplitude * 0.15F + 1e-4F);
        EXPECT_TRUE(glm::all(glm::lessThanEqual(box.max, limit))) << seed;
        EXPECT_TRUE(glm::all(glm::greaterThanEqual(box.min, -limit))) << seed;
        for (const asset::Vertex& v : stone->vertices) {
            EXPECT_NEAR(glm::length(v.normal), 1.0F, 1e-4F);
            EXPECT_NEAR(glm::dot(glm::vec3(v.tangent), v.normal), 0.0F, 1e-3F);
        }
    }
}

TEST(GeometryStone, RefusesBadOptions) {
    StoneOptions o;
    o.size.x = 0.0F;
    EXPECT_ERR(generate_stone(o));
    o = {};
    o.subdivisions = 6;
    EXPECT_ERR(generate_stone(o));
    o = {};
    o.roundness = 2.0F;
    EXPECT_ERR(generate_stone(o));
    o = {};
    o.uv_scale = NAN;
    EXPECT_ERR(generate_stone(o));
}
