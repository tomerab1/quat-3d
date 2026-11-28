// Curve queries (ROADMAP G18.1): closest point (exact cases + a property test
// against brute force), XZ crossings, self-crossings and offsets.

#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include "engine/spline/queries.hpp"

namespace quat::test {
namespace {

namespace sp = engine::spline;

[[nodiscard]] sp::Curve curve_of(sp::SplineKind kind, std::vector<glm::vec3> pts, bool closed = false) {
    sp::Spline s;
    s.kind = kind;
    s.closed = closed;
    for (const glm::vec3& p : pts) s.points.push_back({p, 2.0F, 0.0F, 0});
    auto c = sp::Curve::build(s);
    EXPECT_TRUE(c.has_value());
    return c ? *c : sp::Curve{};
}

// Deterministic pseudo-random floats in [lo, hi) (splitmix64).
struct Rng {
    std::uint64_t state;
    float next(float lo, float hi) {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        return lo + (hi - lo) * static_cast<float>(z >> 40) / static_cast<float>(1ULL << 24);
    }
};

TEST(SplineClosest, ExactOnALine) {
    const sp::Curve line = curve_of(sp::SplineKind::polyline, {{0, 0, 0}, {10, 0, 0}});
    const auto c = sp::closest_point(line, {5, 3, 0});
    ASSERT_TRUE(c.has_value());
    EXPECT_NEAR(c->param, 0.5F, 1e-6F);
    EXPECT_NEAR(c->distance, 5.0F, 1e-5F);
    EXPECT_NEAR(c->gap, 3.0F, 1e-6F);
    EXPECT_NEAR(c->position.x, 5.0F, 1e-5F);
    // Past the end: the end point.
    EXPECT_NEAR(sp::closest_point(line, {14, 0, 3})->param, 1.0F, 0.0F);
    // XZ only ignores height.
    EXPECT_NEAR(sp::closest_point(line, {5, 30, 2}, true)->gap, 2.0F, 1e-6F);
    EXPECT_FALSE(sp::closest_point(sp::Curve{}, {0, 0, 0}).has_value());
}

TEST(SplineClosest, NeverWorseThanBruteForce) {
    const sp::Curve c = curve_of(sp::SplineKind::centripetal_catmull_rom,
                                 {{0, 0, 0}, {6, 1, 2}, {3, 0, 8}, {-4, 2, 5}, {-2, 0, -3}, {5, -1, -6}}, true);
    Rng rng{42};
    for (int q = 0; q < 200; ++q) {
        const glm::vec3 p(rng.next(-12, 12), rng.next(-3, 3), rng.next(-12, 12));
        const auto got = sp::closest_point(c, p);
        ASSERT_TRUE(got.has_value());
        float brute = 1.0e9F;
        for (int i = 0; i <= 6000; ++i) {
            brute = std::min(brute, glm::length(c.position(c.max_param() * static_cast<float>(i) / 6000.0F) - p));
        }
        EXPECT_LE(got->gap, brute + 1e-4F) << "query " << q;
        EXPECT_NEAR(glm::length(got->position - p), got->gap, 1e-4F);
    }
}

TEST(SplineCrossings, TwoLinesCrossOnceAtTheCentre) {
    const sp::Curve a = curve_of(sp::SplineKind::polyline, {{0, 0, 0}, {10, 1, 10}});
    const sp::Curve b = curve_of(sp::SplineKind::polyline, {{0, 5, 10}, {10, 5, 0}});
    const auto x = sp::crossings_xz(a, b);
    ASSERT_EQ(x.size(), 1U);
    EXPECT_NEAR(x[0].param_a, 0.5F, 1e-6F);
    EXPECT_NEAR(x[0].param_b, 0.5F, 1e-6F);
    EXPECT_NEAR(x[0].position_a.x, 5.0F, 1e-5F);
    EXPECT_NEAR(x[0].position_a.y, 0.5F, 1e-5F); // each curve keeps its own height
    EXPECT_NEAR(x[0].position_b.y, 5.0F, 1e-5F);
}

TEST(SplineCrossings, CurveAgainstLineMatchesTheSignChanges) {
    // A wave crossing the X axis: count sign changes of z densely.
    const sp::Curve wave = curve_of(sp::SplineKind::centripetal_catmull_rom,
                                    {{0, 0, -3}, {4, 0, 3}, {8, 0, -3}, {12, 0, 3}, {16, 0, -3}});
    const sp::Curve axis = curve_of(sp::SplineKind::polyline, {{-1, 0, 0}, {17, 0, 0}});
    const auto x = sp::crossings_xz(wave, axis);
    ASSERT_EQ(x.size(), 4U);
    for (const auto& h : x) {
        EXPECT_NEAR(h.position_a.z, 0.0F, 1e-4F);
        EXPECT_NEAR(glm::length(h.position_a - h.position_b), 0.0F, 1e-4F);
    }
    for (std::size_t i = 1; i < x.size(); ++i) EXPECT_LT(x[i - 1].param_a, x[i].param_a);
}

TEST(SplineCrossings, SelfCrossingsOfAFigureEight) {
    const sp::Curve eight = curve_of(sp::SplineKind::centripetal_catmull_rom,
                                     {{0, 0, 0}, {5, 0, 5}, {10, 0, 0}, {5, 0, -5}, {0, 0, 0}, {-5, 0, 5},
                                      {-10, 0, 0}, {-5, 0, -5}},
                                     true);
    const auto x = sp::self_crossings_xz(eight);
    // The loop passes through the origin twice (points 0 and 4) and crosses there.
    ASSERT_EQ(x.size(), 1U);
    EXPECT_NEAR(glm::length(x[0].position_a), 0.0F, 1e-3F);
    EXPECT_LT(x[0].param_a, x[0].param_b);

    const sp::Curve ring = curve_of(sp::SplineKind::centripetal_catmull_rom,
                                    {{10, 0, 0}, {0, 0, 10}, {-10, 0, 0}, {0, 0, -10}}, true);
    EXPECT_TRUE(sp::self_crossings_xz(ring).empty());
    // Centripetal Catmull-Rom never loops within a segment, even with an
    // uneven point spacing that makes uniform Catmull-Rom self-intersect.
    const sp::Curve tight = curve_of(sp::SplineKind::centripetal_catmull_rom,
                                     {{0, 0, 0}, {0, 0, 10}, {0.5F, 0, 10}, {10, 0, 0}});
    EXPECT_TRUE(sp::self_crossings_xz(tight).empty());
}

TEST(SplineOffset, StraightOffsetsAreExact) {
    const sp::Curve line = curve_of(sp::SplineKind::polyline, {{0, 1, 0}, {10, 1, 0}});
    const auto right = sp::offset_xz(line, {.distance = 2.0F, .spacing = 2.5F});
    ASSERT_EQ(right.size(), 5U);
    for (const glm::vec3& p : right) {
        EXPECT_NEAR(p.z, 2.0F, 1e-6F);
        EXPECT_EQ(p.y, 1.0F);
    }
    EXPECT_EQ(right.front().x, 0.0F);
    EXPECT_EQ(right.back().x, 10.0F);
    // The left edge of a path of the curve's width (2 m): one half width left.
    const auto edge = sp::offset_xz(line, {.width_factor = -1.0F, .spacing = 5.0F});
    EXPECT_NEAR(edge[1].z, -1.0F, 1e-6F);
}

TEST(SplineOffset, InnerOffsetOfATightBendHasNoLoops) {
    // A sharp V: the inner offset of both legs overshoots the corner and
    // crosses itself (a swallowtail). Travel turns from +X toward +Z, so the
    // inside of the bend is on the right (+distance).
    const sp::Curve vee = curve_of(sp::SplineKind::polyline, {{0, 0, 0}, {10, 0, 0}, {0, 0, 6}});
    const auto raw = sp::offset_xz(vee, {.distance = 1.0F, .spacing = 0.1F, .trim_loops = false});
    const auto trimmed = sp::offset_xz(vee, {.distance = 1.0F, .spacing = 0.1F});
    auto self_hits = [](const std::vector<glm::vec3>& pts) {
        sp::Spline s;
        s.kind = sp::SplineKind::polyline;
        for (const glm::vec3& p : pts) s.points.push_back({p, 1.0F, 0.0F, 0});
        return sp::self_crossings_xz(*sp::Curve::build(s)).size();
    };
    EXPECT_GT(self_hits(raw), 0U);
    EXPECT_EQ(self_hits(trimmed), 0U);
    EXPECT_LT(trimmed.size(), raw.size());
    // Every trimmed point keeps (at least) its distance from the curve.
    for (const glm::vec3& p : trimmed) EXPECT_GE(sp::closest_point(vee, p, true)->gap, 1.0F - 1e-3F);

    // Offsets outward of a loop stay at the offset distance from the curve.
    const sp::Curve ring = curve_of(sp::SplineKind::centripetal_catmull_rom,
                                    {{10, 0, 0}, {0, 0, 10}, {-10, 0, 0}, {0, 0, -10}}, true);
    // At (10, 0, 0) the ring travels along +Z, so right (tangent x up) points
    // to -X, inward: outward is a negative distance.
    const auto outer = sp::offset_xz(ring, {.distance = -1.5F, .spacing = 0.5F});
    for (const glm::vec3& p : outer) EXPECT_NEAR(sp::closest_point(ring, p, true)->gap, 1.5F, 2e-3F);
}

TEST(SplineFlatten, StaysWithinTolerance) {
    const sp::Curve c = curve_of(sp::SplineKind::centripetal_catmull_rom,
                                 {{0, 0, 0}, {5, 0, 4}, {9, 0, -3}, {14, 0, 2}});
    const auto pts = sp::flatten(c, 0.01F);
    ASSERT_GE(pts.size(), 2U);
    EXPECT_EQ(pts.front().param, 0.0F);
    EXPECT_EQ(pts.back().param, c.max_param());
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        // The curve between two flattened points stays near their chord.
        const float tm = 0.5F * (pts[i].param + pts[i + 1].param);
        const glm::vec3 mid = c.position(tm);
        const glm::vec3 a = pts[i].position;
        const glm::vec3 d = pts[i + 1].position - a;
        const float u = glm::clamp(glm::dot(mid - a, d) / glm::max(glm::dot(d, d), 1e-12F), 0.0F, 1.0F);
        EXPECT_LE(glm::length(mid - (a + d * u)), 0.01F + 1e-5F);
    }
}

} // namespace
} // namespace quat::test
