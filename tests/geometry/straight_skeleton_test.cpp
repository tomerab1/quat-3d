#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/geometry/straight_skeleton.hpp"
#include "geometry/geometry_test_util.hpp"
#include "support/expect_ok.hpp"

using namespace engine::geometry;
using quat::test::geometry::random_star;
using quat::test::geometry::rect;

namespace {

// Invariants every skeleton satisfies: input nodes first at time 0, one
// face per edge starting with that edge, face nodes distinct, faces of
// moving edges tiling the polygon.
void check_invariants(const StraightSkeleton& sk, const Polygon& p) {
    const std::size_t inputs = sk.edge_weights.size();
    ASSERT_EQ(sk.faces.size(), inputs);
    ASSERT_EQ(sk.source_edges.size(), inputs);
    for (std::size_t n = 0; n < sk.nodes.size(); ++n) {
        if (n < inputs) {
            EXPECT_EQ(sk.nodes[n].time, 0.0);
        } else {
            EXPECT_GT(sk.nodes[n].time, 0.0);
        }
    }
    double tiled = 0.0;
    for (std::size_t e = 0; e < sk.faces.size(); ++e) {
        const SkeletonFace& f = sk.faces[e];
        EXPECT_EQ(f.edge, e);
        ASSERT_GE(f.nodes.size(), 3U);
        EXPECT_EQ(f.nodes[0], e);
        std::vector<std::uint32_t> sorted = f.nodes;
        std::sort(sorted.begin(), sorted.end());
        EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end()) << "face " << e;
        std::vector<glm::dvec2> plan;
        for (const std::uint32_t n : f.nodes) {
            plan.push_back(sk.nodes[n].position);
        }
        const double a = signed_area(plan);
        if (sk.edge_weights[e] > 0.0) {
            EXPECT_GT(a, 0.0) << "face " << e;
        }
        tiled += a;
    }
    EXPECT_NEAR(tiled, area(p), 1e-9 * area(p));
}

std::size_t count_nodes_at(const StraightSkeleton& sk, glm::dvec2 at, double time) {
    return static_cast<std::size_t>(std::count_if(sk.nodes.begin(), sk.nodes.end(), [&](const SkeletonNode& n) {
        return glm::length(n.position - at) < 1e-9 && std::fabs(n.time - time) < 1e-9;
    }));
}

} // namespace

TEST(GeometryStraightSkeleton, SquareMeetsInTheCentre) {
    const Polygon p{rect(0, 0, 2, 2), {}};
    auto sk = straight_skeleton(p);
    ASSERT_OK(sk);
    check_invariants(*sk, p);
    EXPECT_EQ(sk->nodes.size(), 5U);
    EXPECT_EQ(count_nodes_at(*sk, {1, 1}, 1.0), 1U);
    EXPECT_DOUBLE_EQ(sk->max_time, 1.0);
    for (const SkeletonFace& f : sk->faces) {
        EXPECT_EQ(f.nodes.size(), 3U); // four triangles (a pyramid)
    }
}

TEST(GeometryStraightSkeleton, RectangleHasARidge) {
    const Polygon p{rect(0, 0, 4, 2), {}};
    auto sk = straight_skeleton(p);
    ASSERT_OK(sk);
    check_invariants(*sk, p);
    EXPECT_EQ(count_nodes_at(*sk, {1, 1}, 1.0), 1U);
    EXPECT_EQ(count_nodes_at(*sk, {3, 1}, 1.0), 1U);
    EXPECT_EQ(sk->faces[0].nodes.size(), 4U); // long sides: trapezoids
    EXPECT_EQ(sk->faces[1].nodes.size(), 3U); // short sides: triangles
}

TEST(GeometryStraightSkeleton, RectilinearFootprints) {
    struct Case {
        const char* name;
        Polygon polygon;
        std::size_t nodes;
    };
    const std::vector<Case> cases{
        {"L", {{{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}}, {}}, 9},
        {"T", {{{0, 0}, {6, 0}, {6, 2}, {4, 2}, {4, 5}, {2, 5}, {2, 2}, {0, 2}}, {}}, 12},
        {"U", {{{0, 0}, {6, 0}, {6, 4}, {4, 4}, {4, 2}, {2, 2}, {2, 4}, {0, 4}}, {}}, 12},
        {"plus",
         {{{2, 0}, {4, 0}, {4, 2}, {6, 2}, {6, 4}, {4, 4}, {4, 6}, {2, 6}, {2, 4}, {0, 4}, {0, 2}, {2, 2}}, {}},
         17},
        {"courtyard", {rect(0, 0, 10, 10), {{{3, 3}, {3, 7}, {7, 7}, {7, 3}}}}, 12},
    };
    for (const Case& c : cases) {
        auto sk = straight_skeleton(c.polygon);
        ASSERT_OK(sk) << c.name;
        check_invariants(*sk, c.polygon);
        EXPECT_EQ(sk->nodes.size(), c.nodes) << c.name;
    }
    // The plus collapses onto its centre: one node there, where all four
    // reflex corners meet (a generalised intersection event).
    auto plus = straight_skeleton(cases[3].polygon);
    ASSERT_OK(plus);
    EXPECT_EQ(count_nodes_at(*plus, {3, 3}, 1.0), 1U);
}

TEST(GeometryStraightSkeleton, GablesAndShedRoofs) {
    // Weight 0 on the short ends: they never move (vertical gables).
    const Polygon p{rect(0, 0, 4, 2), {}};
    auto gable = straight_skeleton(p, std::vector<double>{1, 0, 1, 0});
    ASSERT_OK(gable);
    check_invariants(*gable, p);
    EXPECT_EQ(count_nodes_at(*gable, {0, 1}, 1.0), 1U); // ridge runs end to end
    EXPECT_EQ(count_nodes_at(*gable, {4, 1}, 1.0), 1U);
    // Only one moving edge: a shed roof rising to the opposite wall.
    auto shed = straight_skeleton(p, std::vector<double>{1, 0, 0, 0});
    ASSERT_OK(shed);
    check_invariants(*shed, p);
    EXPECT_DOUBLE_EQ(shed->max_time, 2.0);
    EXPECT_EQ(shed->faces[0].nodes.size(), 4U);
}

TEST(GeometryStraightSkeleton, WeightsScaleTheFronts) {
    const Polygon p{rect(0, 0, 4, 2), {}};
    // Bottom twice as fast as the top: they meet at y = 4/3 at t = 2/3.
    auto sk = straight_skeleton(p, std::vector<double>{2, 1, 1, 1});
    ASSERT_OK(sk);
    check_invariants(*sk, p);
    EXPECT_NEAR(sk->max_time, 2.0 / 3.0, 1e-12);
    for (const SkeletonNode& n : sk->nodes) {
        if (n.time > 0.0) {
            EXPECT_NEAR(n.position.y, 4.0 / 3.0, 1e-12);
        }
    }
    // Uniform scaling of every weight scales time only.
    auto doubled = straight_skeleton(p, std::vector<double>{2, 2, 2, 2});
    ASSERT_OK(doubled);
    EXPECT_NEAR(doubled->max_time, 0.5, 1e-12);
}

TEST(GeometryStraightSkeleton, MergesCollinearAndDuplicatePoints) {
    const Polygon p{{{0, 0}, {1, 0}, {2, 0}, {2, 0}, {2, 2}, {0, 2}}, {}};
    auto sk = straight_skeleton(p);
    ASSERT_OK(sk);
    EXPECT_EQ(sk->edge_weights.size(), 4U);
    EXPECT_EQ(sk->source_edges, (std::vector<std::uint32_t>{0, 3, 4, 5}));
}

TEST(GeometryStraightSkeleton, RefusesInvalidInput) {
    EXPECT_ERR(straight_skeleton(Polygon{{{0, 0}, {0, 2}, {2, 2}, {2, 0}}, {}})); // clockwise
    EXPECT_ERR(straight_skeleton(Polygon{{{0, 0}, {2, 2}, {2, 0}, {0, 2}}, {}})); // self-intersecting
    EXPECT_ERR(straight_skeleton(Polygon{{{0, 0}, {1, 0}}, {}}));
    EXPECT_ERR(straight_skeleton(Polygon{rect(0, 0, 1, 1), {}}, std::vector<double>{1, 1}));
    EXPECT_ERR(straight_skeleton(Polygon{rect(0, 0, 1, 1), {}}, std::vector<double>{1, -1, 1, 1}));
    EXPECT_ERR(straight_skeleton(Polygon{rect(0, 0, 1, 1), {}}, std::vector<double>{0, 0, 0, 0}));
    EXPECT_ERR(straight_skeleton(Polygon{rect(0, 0, 1, 1), {{{5, 5}, {5, 6}, {6, 6}, {6, 5}}}})); // hole outside
    // Collinear edges with different weights: ambiguous (Biedl et al. 2015).
    auto ambiguous = straight_skeleton(Polygon{{{0, 0}, {1, 0}, {2, 0}, {2, 1}, {0, 1}}, {}},
                                       std::vector<double>{1, 2, 1, 1, 1});
    ASSERT_ERR(ambiguous);
    EXPECT_NE(ambiguous.error().message.find("ambiguous"), std::string::npos);
    // A region only weight-0 edges enclose is never swept.
    auto stalled = straight_skeleton(Polygon{{{3, 7}, {8, 7}, {8, 8}, {7, 8}, {7, 9}, {3, 9}}, {}},
                                     std::vector<double>{0, 0, 0, 1, 0, 0});
    ASSERT_ERR(stalled);
    EXPECT_NE(stalled.error().message.find("weight-0"), std::string::npos);
}

TEST(GeometryStraightSkeleton, RegularPolygonMeetsAtItsInradius) {
    for (int n : {3, 5, 6, 12, 64}) {
        Polygon p;
        for (int i = 0; i < n; ++i) {
            const double a = 6.283185307179586 * i / n;
            p.outer.push_back({std::cos(a), std::sin(a)});
        }
        auto sk = straight_skeleton(p);
        ASSERT_OK(sk) << n;
        check_invariants(*sk, p);
        EXPECT_NEAR(sk->max_time, std::cos(3.141592653589793 / n), 1e-9) << n;
    }
}

TEST(GeometryStraightSkeleton, FuzzStarsAndWeights) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> weight(0.3, 2.3);
    for (int k = 0; k < 400; ++k) {
        const Polygon p = random_star(rng, 3 + static_cast<int>(rng() % 40));
        std::vector<double> w;
        if (k % 2 == 1) {
            for (std::size_t i = 0; i < p.outer.size(); ++i) {
                w.push_back(weight(rng));
            }
        }
        auto sk = straight_skeleton(p, w);
        ASSERT_OK(sk) << "case " << k;
        check_invariants(*sk, p);
    }
}

TEST(GeometryStraightSkeleton, FuzzRectangleUnions) {
    // Overlapping rectangles: rectilinear footprints full of simultaneous
    // events, holes, and vertices where rings touch.
    std::mt19937 rng(99);
    int checked = 0;
    for (int k = 0; k < 600; ++k) {
        std::vector<Ring> rings;
        const int count = 1 + static_cast<int>(rng() % 6);
        for (int i = 0; i < count; ++i) {
            rings.push_back(rect(rng() % 8, rng() % 8, 1 + rng() % 4, 1 + rng() % 4));
        }
        auto set = repair(rings);
        ASSERT_OK(set);
        for (const Polygon& p : *set) {
            auto sk = straight_skeleton(p);
            ASSERT_OK(sk) << "case " << k;
            check_invariants(*sk, p);
            ++checked;
        }
    }
    EXPECT_GT(checked, 600);
}
