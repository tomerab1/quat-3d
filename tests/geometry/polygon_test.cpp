#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "engine/geometry/polygon.hpp"
#include "geometry/geometry_test_util.hpp"
#include "support/expect_ok.hpp"

using namespace engine::geometry;
using quat::test::geometry::rect;

TEST(GeometryPolygon, AreaAndContains) {
    const Polygon p{rect(0, 0, 4, 3), {{{1, 1}, {1, 2}, {2, 2}, {2, 1}}}};
    EXPECT_DOUBLE_EQ(signed_area(p.outer), 12.0);
    EXPECT_DOUBLE_EQ(signed_area(p.holes[0]), -1.0);
    EXPECT_DOUBLE_EQ(area(p), 11.0);
    EXPECT_TRUE(contains(p, {3.0, 2.5}));
    EXPECT_FALSE(contains(p, {1.5, 1.5})); // in the hole
    EXPECT_FALSE(contains(p, {5.0, 1.0}));
}

TEST(GeometryPolygon, RepairOrientsAndSplitsSelfIntersections) {
    // Clockwise square: repaired to counter-clockwise.
    std::vector<Ring> cw{{{0, 0}, {0, 2}, {2, 2}, {2, 0}}};
    auto r = repair(cw);
    ASSERT_OK(r);
    ASSERT_EQ(r->size(), 1U);
    EXPECT_GT(signed_area((*r)[0].outer), 0.0);
    EXPECT_DOUBLE_EQ(area((*r)[0]), 4.0);
    // Starts at the lexicographically smallest point.
    EXPECT_EQ((*r)[0].outer[0], glm::dvec2(0.0, 0.0));
    // A bow tie becomes two triangles.
    std::vector<Ring> bowtie{{{0, 0}, {2, 2}, {2, 0}, {0, 2}}};
    auto b = repair(bowtie);
    ASSERT_OK(b);
    ASSERT_EQ(b->size(), 2U);
    EXPECT_NEAR(area((*b)[0]) + area((*b)[1]), 2.0, 1e-9);
    // Nested rings under even-odd: a square with a hole.
    std::vector<Ring> nested{rect(0, 0, 4, 4), rect(1, 1, 2, 2)};
    auto n = repair(nested, FillRule::even_odd);
    ASSERT_OK(n);
    ASSERT_EQ(n->size(), 1U);
    ASSERT_EQ((*n)[0].holes.size(), 1U);
    EXPECT_LT(signed_area((*n)[0].holes[0]), 0.0);
    EXPECT_DOUBLE_EQ(area((*n)[0]), 12.0);
}

TEST(GeometryPolygon, RefusesBadCoordinates) {
    std::vector<Ring> nan_ring{{{0, 0}, {1, 0}, {0, std::nan("")}}};
    EXPECT_ERR(repair(nan_ring));
    std::vector<Ring> far{{{0, 0}, {2e6, 0}, {0, 1}}};
    EXPECT_ERR(repair(far));
    EXPECT_ERR(offset({Polygon{rect(0, 0, 1, 1), {}}}, INFINITY));
    OffsetOptions bad;
    bad.miter_limit = 0.5;
    EXPECT_ERR(offset({Polygon{rect(0, 0, 1, 1), {}}}, 0.1, bad));
    EXPECT_ERR(simplify({Polygon{rect(0, 0, 1, 1), {}}}, -1.0));
}

TEST(GeometryPolygon, OffsetGrowsAndShrinks) {
    const PolygonSet square{Polygon{rect(0, 0, 4, 4), {}}};
    auto grown = offset(square, 0.5);
    ASSERT_OK(grown);
    ASSERT_EQ(grown->size(), 1U);
    EXPECT_NEAR(area((*grown)[0]), 25.0, 1e-6); // miter: a 5x5 square
    auto shrunk = offset(square, -1.0);
    ASSERT_OK(shrunk);
    ASSERT_EQ(shrunk->size(), 1U);
    EXPECT_NEAR(area((*shrunk)[0]), 4.0, 1e-6);
    auto gone = offset(square, -2.5);
    ASSERT_OK(gone);
    EXPECT_TRUE(gone->empty());
    OffsetOptions round;
    round.join = JoinType::round;
    auto rounded = offset(square, 1.0, round);
    ASSERT_OK(rounded);
    // 4x4 square grown by 1 with round corners: 16 + 4*4 + pi.
    EXPECT_NEAR(area((*rounded)[0]), 32.0 + 3.14159265358979, 0.05);
    // A dumbbell splits in two when shrunk past its neck.
    std::vector<Ring> dumbbell{{{0, 0}, {3, 0}, {3, 1}, {4, 1}, {4, 0}, {7, 0}, {7, 3}, {4, 3}, {4, 2}, {3, 2}, {3, 3}, {0, 3}}};
    auto d = repair(dumbbell);
    ASSERT_OK(d);
    auto split = offset(*d, -0.75);
    ASSERT_OK(split);
    EXPECT_EQ(split->size(), 2U);
}

TEST(GeometryPolygon, Booleans) {
    const PolygonSet a{Polygon{rect(0, 0, 4, 4), {}}};
    const PolygonSet b{Polygon{rect(2, 2, 4, 4), {}}};
    auto u = boolean(BooleanOp::unite, a, b);
    auto i = boolean(BooleanOp::intersect, a, b);
    auto s = boolean(BooleanOp::subtract, a, b);
    auto x = boolean(BooleanOp::exclusive_or, a, b);
    ASSERT_OK(u);
    ASSERT_OK(i);
    ASSERT_OK(s);
    ASSERT_OK(x);
    auto total = [](const PolygonSet& set) {
        double t = 0.0;
        for (const Polygon& p : set) {
            t += area(p);
        }
        return t;
    };
    EXPECT_DOUBLE_EQ(total(*u), 28.0);
    EXPECT_DOUBLE_EQ(total(*i), 4.0);
    EXPECT_DOUBLE_EQ(total(*s), 12.0);
    EXPECT_DOUBLE_EQ(total(*x), 24.0);
    // Subtracting an inner square makes a hole.
    auto ring = boolean(BooleanOp::subtract, a, {Polygon{rect(1, 1, 2, 2), {}}});
    ASSERT_OK(ring);
    ASSERT_EQ(ring->size(), 1U);
    EXPECT_EQ((*ring)[0].holes.size(), 1U);
}

TEST(GeometryPolygon, SimplifyDropsNearCollinearPoints) {
    const PolygonSet wobbly{Polygon{{{0, 0}, {2, 0.001}, {4, 0}, {4, 4}, {0, 4}}, {}}};
    auto s = simplify(wobbly, 0.01);
    ASSERT_OK(s);
    ASSERT_EQ(s->size(), 1U);
    EXPECT_EQ((*s)[0].outer.size(), 4U);
}

TEST(GeometryPolygon, CanonicalOutputIsStable) {
    const PolygonSet a{Polygon{rect(5, 5, 2, 2), {}}, Polygon{rect(0, 0, 1, 1), {}}};
    auto first = boolean(BooleanOp::unite, a, {});
    ASSERT_OK(first);
    ASSERT_EQ(first->size(), 2U);
    EXPECT_EQ((*first)[0].outer[0], glm::dvec2(0.0, 0.0)); // sorted by first point
    auto again = boolean(BooleanOp::unite, *first, {});
    ASSERT_OK(again);
    ASSERT_EQ(again->size(), first->size());
    for (std::size_t k = 0; k < first->size(); ++k) {
        EXPECT_EQ((*again)[k].outer, (*first)[k].outer);
    }
}
