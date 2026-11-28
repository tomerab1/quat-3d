#include <cmath>
#include <random>

#include <gtest/gtest.h>

#include "engine/geometry/triangulate.hpp"
#include "geometry/geometry_test_util.hpp"
#include "support/expect_ok.hpp"

using namespace engine::geometry;
using quat::test::geometry::random_star;
using quat::test::geometry::rect;

namespace {

double covered(const Triangulation& t) {
    double a = 0.0;
    for (std::size_t k = 0; k + 2 < t.indices.size(); k += 3) {
        const glm::dvec2 p = t.points[t.indices[k]];
        const glm::dvec2 q = t.points[t.indices[k + 1]];
        const glm::dvec2 r = t.points[t.indices[k + 2]];
        const double c = (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
        EXPECT_GT(c, 0.0) << "triangle " << k / 3 << " is not counter-clockwise";
        a += 0.5 * c;
    }
    return a;
}

} // namespace

TEST(GeometryTriangulate, SquareWithHole) {
    const Polygon p{rect(0, 0, 4, 4), {{{1, 1}, {1, 3}, {3, 3}, {3, 1}}}};
    auto t = triangulate(p);
    ASSERT_OK(t);
    EXPECT_EQ(t->points.size(), 8U);
    EXPECT_EQ(t->indices.size(), 8U * 3U); // n + 2h - 2 triangles
    EXPECT_DOUBLE_EQ(covered(*t), 12.0);
}

TEST(GeometryTriangulate, AcceptsClockwiseAndCollinearInput) {
    const Polygon cw{{{0, 0}, {0, 2}, {1, 2}, {2, 2}, {2, 0}}, {}};
    auto t = triangulate(cw);
    ASSERT_OK(t);
    EXPECT_DOUBLE_EQ(covered(*t), 4.0);
}

TEST(GeometryTriangulate, RefusesDegenerateInput) {
    EXPECT_ERR(triangulate(Polygon{{{0, 0}, {1, 1}}, {}}));
    EXPECT_ERR(triangulate(Polygon{{{0, 0}, {1, 0}, {0, INFINITY}}, {}}));
    // Self-intersecting: triangles cannot cover the signed area.
    EXPECT_ERR(triangulate(Polygon{{{0, 0}, {2, 2}, {2, 0}, {0, 2}}, {}}));
}

TEST(GeometryTriangulate, FuzzStarsCoverTheirArea) {
    std::mt19937 rng(42);
    for (int k = 0; k < 500; ++k) {
        const Polygon p = random_star(rng, 3 + static_cast<int>(rng() % 60));
        auto t = triangulate(p);
        ASSERT_OK(t) << "star " << k;
        EXPECT_NEAR(covered(*t), signed_area(p.outer), 1e-9 * signed_area(p.outer)) << "star " << k;
    }
}
