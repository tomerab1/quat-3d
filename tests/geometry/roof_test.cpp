#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "engine/geometry/straight_skeleton.hpp"
#include "geometry/geometry_test_util.hpp"
#include "support/expect_ok.hpp"

using namespace engine::geometry;
namespace asset = engine::asset;
using quat::test::geometry::rect;
using quat::test::geometry::surface_area;
using quat::test::geometry::triangle_normal;

namespace {

double submesh_area(const MeshData& m, const asset::SubMesh& s) {
    double a = 0.0;
    for (std::uint32_t t = s.index_offset / 3; t < (s.index_offset + s.index_count) / 3; ++t) {
        a += quat::test::geometry::triangle_area(m, t);
    }
    return a;
}

} // namespace

TEST(GeometryRoof, HipRoofAt45Degrees) {
    const Polygon p{{{0, 0}, {8, 0}, {8, 4}, {4, 4}, {4, 8}, {0, 8}}, {}};
    auto sk = straight_skeleton(p);
    ASSERT_OK(sk);
    RoofOptions options;
    options.base_height = 3.0;
    auto roof = roof_mesh(*sk, options);
    ASSERT_OK(roof);
    ASSERT_OK(validate(*roof));
    // Every face pitched at 45 degrees: surface = plan area * sqrt(2).
    EXPECT_NEAR(surface_area(*roof), area(p) * std::sqrt(2.0), 1e-4);
    const asset::Aabb box = compute_bounds(*roof);
    EXPECT_FLOAT_EQ(box.min.y, 3.0F);
    EXPECT_FLOAT_EQ(box.max.y, 5.0F); // arm width 4: ridge 2 m up
    for (std::size_t t = 0; t < roof->triangle_count(); ++t) {
        const glm::dvec3 n = triangle_normal(*roof, t);
        EXPECT_NEAR(n.y, std::sqrt(0.5), 1e-5) << "triangle " << t; // 45 degrees, facing up
        const glm::vec3 vn = roof->vertices[roof->indices[3 * t]].normal;
        EXPECT_NEAR(glm::dot(glm::dvec3(vn), n), 1.0, 1e-5); // flat normals match the winding
    }
    for (const asset::Vertex& v : roof->vertices) {
        EXPECT_NEAR(glm::dot(glm::vec3(v.tangent), v.normal), 0.0F, 1e-4F); // MikkTSpace ran
    }
}

TEST(GeometryRoof, GableEndsAreVerticalAndSeparateMaterial) {
    const Polygon p{rect(0, 0, 6, 4), {}};
    auto sk = straight_skeleton(p, std::vector<double>{1, 0, 1, 0});
    ASSERT_OK(sk);
    RoofOptions options;
    options.material = 3;
    options.gable_material = 7;
    options.height_scale = 0.5; // pitch atan(0.5)
    auto roof = roof_mesh(*sk, options);
    ASSERT_OK(roof);
    ASSERT_EQ(roof->submeshes.size(), 2U);
    EXPECT_EQ(roof->submeshes[0].material, 3U);
    EXPECT_EQ(roof->submeshes[1].material, 7U);
    // Gables: two triangles of base 4 and height 2 * 0.5 = 1.
    EXPECT_NEAR(submesh_area(*roof, roof->submeshes[1]), 4.0, 1e-5);
    for (std::uint32_t t = roof->submeshes[1].index_offset / 3; t < roof->triangle_count(); ++t) {
        const glm::dvec3 n = triangle_normal(*roof, t);
        EXPECT_NEAR(n.y, 0.0, 1e-6);
        EXPECT_NEAR(std::fabs(n.x), 1.0, 1e-6); // the x = 0 and x = 6 ends, facing out
        const glm::dvec3 a(roof->vertices[roof->indices[3 * t]].position);
        EXPECT_GT(n.x * (a.x - 3.0), 0.0);
    }
    // Slopes: 2 planes, 6 long, rising 1 over 2.
    EXPECT_NEAR(submesh_area(*roof, roof->submeshes[0]), 2.0 * 6.0 * std::sqrt(4.0 + 1.0), 1e-4);
}

TEST(GeometryRoof, FlatTopCutsAtMaxHeight) {
    const Polygon p{rect(0, 0, 10, 6), {}};
    auto sk = straight_skeleton(p);
    ASSERT_OK(sk);
    RoofOptions options;
    options.max_height = 1.0;
    auto roof = roof_mesh(*sk, options);
    ASSERT_OK(roof);
    ASSERT_OK(validate(*roof));
    const asset::Aabb box = compute_bounds(*roof);
    EXPECT_FLOAT_EQ(box.max.y, 1.0F);
    double top = 0.0;
    for (std::size_t t = 0; t < roof->triangle_count(); ++t) {
        const glm::dvec3 n = triangle_normal(*roof, t);
        EXPECT_GT(n.y, 0.0);
        if (n.y > 0.999) {
            top += quat::test::geometry::triangle_area(*roof, t);
        }
    }
    EXPECT_NEAR(top, 8.0 * 4.0, 1e-3); // inset by 1
}

TEST(GeometryRoof, RefusesBadOptions) {
    auto sk = straight_skeleton(Polygon{rect(0, 0, 2, 2), {}});
    ASSERT_OK(sk);
    RoofOptions bad;
    bad.height_scale = 0.0;
    EXPECT_ERR(roof_mesh(*sk, bad));
    bad = {};
    bad.uv_scale = NAN;
    EXPECT_ERR(roof_mesh(*sk, bad));
    auto empty = roof_mesh(StraightSkeleton{}, RoofOptions{});
    ASSERT_OK(empty);
    EXPECT_TRUE(empty->empty());
}
