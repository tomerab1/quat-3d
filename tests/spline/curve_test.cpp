// Spline compilation, evaluation, arc length, bounds and even spacing
// (ROADMAP G18.1), against hand-derived and brute-force reference values.

#include <cmath>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "engine/spline/curve.hpp"
#include "engine/spline/spline.hpp"

namespace quat::test {
namespace {

namespace sp = engine::spline;

[[nodiscard]] sp::Spline make(sp::SplineKind kind, std::initializer_list<glm::vec3> pts, bool closed = false) {
    sp::Spline s;
    s.kind = kind;
    s.closed = closed;
    for (const glm::vec3& p : pts) s.points.push_back({p, 1.0F, 0.0F, 0});
    return s;
}

[[nodiscard]] sp::Curve build(const sp::Spline& s) {
    auto c = sp::Curve::build(s);
    EXPECT_TRUE(c.has_value()) << (c ? "" : c.error().message);
    return c ? *c : sp::Curve{};
}

// Arc length by summing 20000 chords per segment, evaluated in double.
[[nodiscard]] double brute_length(const sp::Curve& c) {
    constexpr int k_steps = 20000;
    double sum = 0.0;
    for (const sp::Segment& s : c.segments()) {
        auto at = [&s](double u) {
            const double v = 1.0 - u;
            return glm::dvec3(s.p[0]) * (v * v * v) + glm::dvec3(s.p[1]) * (3.0 * v * v * u) +
                   glm::dvec3(s.p[2]) * (3.0 * v * u * u) + glm::dvec3(s.p[3]) * (u * u * u);
        };
        glm::dvec3 prev = at(0.0);
        for (int i = 1; i <= k_steps; ++i) {
            const glm::dvec3 p = at(static_cast<double>(i) / k_steps);
            sum += glm::length(p - prev);
            prev = p;
        }
    }
    return sum;
}

void expect_vec_near(const glm::vec3& a, const glm::vec3& b, float eps) {
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}

TEST(SplineCompile, CentripetalCatmullRomTangentsMatchTheKnotFormula) {
    // Distances 4, 1, 4 give knot gaps sqrt(4) = 2, 1, 2 exactly. The
    // non-uniform tangents at P1 and P2 (Yuksel et al. 2011) are then
    // m1 = (P1-P0)/2 - (P2-P0)/3 + (P2-P1) = (2/3, 0, 2/3) and likewise
    // m2 = (2/3, 0, 2/3), so the Bézier handles are P1 + m1/3, P2 - m2/3.
    const sp::Spline s = make(sp::SplineKind::centripetal_catmull_rom,
                              {{0, 0, -4}, {0, 0, 0}, {1, 0, 0}, {1, 0, 4}});
    const auto segs = sp::build_segments(s);
    ASSERT_EQ(segs.size(), 3U);
    const sp::Segment& mid = segs[1];
    expect_vec_near(mid.p[0], {0, 0, 0}, 0.0F);
    expect_vec_near(mid.p[1], {2.0F / 9.0F, 0, 2.0F / 9.0F}, 1e-6F);
    expect_vec_near(mid.p[2], {7.0F / 9.0F, 0, -2.0F / 9.0F}, 1e-6F);
    expect_vec_near(mid.p[3], {1, 0, 0}, 0.0F);
}

TEST(SplineCompile, InterpolatesEveryPointExactly) {
    const sp::Spline s = make(sp::SplineKind::centripetal_catmull_rom,
                              {{0, 0, 0}, {3, 1, 2}, {5, 0, -1}, {9, 2, 4}, {12, 0, 0}});
    const sp::Curve c = build(s);
    ASSERT_EQ(c.segment_count(), 4U);
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        EXPECT_EQ(c.position(static_cast<float>(i)), s.points[i].position) << "point " << i;
    }
}

TEST(SplineCompile, CoincidentPointsDoNotProduceNaN) {
    const sp::Spline s = make(sp::SplineKind::centripetal_catmull_rom,
                              {{0, 0, 0}, {0, 0, 0}, {4, 0, 0}, {4, 0, 0}, {4, 0, 3}});
    const sp::Curve c = build(s);
    for (int i = 0; i <= 40; ++i) {
        const sp::CurveSample smp = c.sample(static_cast<float>(i) * 0.1F);
        EXPECT_TRUE(std::isfinite(smp.position.x) && std::isfinite(smp.tangent.x)) << i;
        EXPECT_NEAR(glm::length(smp.tangent), 1.0F, 1e-5F);
    }
    EXPECT_NEAR(c.length(), 7.0F, 0.2F);
}

TEST(SplineCompile, ClosedCurvesWrapAndBezierNeedsThreeKPlusOne) {
    const sp::Spline loop = make(sp::SplineKind::centripetal_catmull_rom,
                                 {{10, 0, 0}, {0, 0, 10}, {-10, 0, 0}, {0, 0, -10}}, true);
    const sp::Curve c = build(loop);
    ASSERT_EQ(c.segment_count(), 4U);
    EXPECT_EQ(c.position(4.0F), c.position(0.0F));
    // Symmetric loop: tangent at a point is perpendicular to its radius.
    EXPECT_NEAR(glm::dot(c.sample(1.0F).tangent, glm::vec3(0, 0, 1)), 0.0F, 1e-6F);
    // Wrapped parameters and distances.
    expect_vec_near(c.position(5.5F), c.position(1.5F), 1e-6F);
    expect_vec_near(c.sample_at(c.length() + 1.0F).position, c.sample_at(1.0F).position, 1e-4F);

    sp::Spline bad = make(sp::SplineKind::cubic_bezier, {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {4, 0, 0}});
    auto r = sp::Curve::build(bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().message,
              "points: a cubic_bezier spline needs 3k + 1 points (anchor, handle, handle, anchor, ...), got 5");
    bad.closed = true;
    bad.points.pop_back();
    bad.points.pop_back();
    EXPECT_TRUE(sp::Curve::build(bad).has_value()); // 3 points closed: one segment back to the start
}

TEST(SplineValidate, NamesTheOffendingField) {
    sp::Spline s = make(sp::SplineKind::polyline, {{0, 0, 0}});
    EXPECT_EQ(sp::validate(s).error().message, "points: a polyline spline needs at least 2 points, got 1");
    s = make(sp::SplineKind::polyline, {{0, 0, 0}, {1, 0, 0}, {2, std::nanf(""), 0}});
    EXPECT_EQ(sp::validate(s).error().message, "points[2].position: expected finite coordinates within +-1e6 m");
    s = make(sp::SplineKind::polyline, {{0, 0, 0}, {1, 0, 0}});
    s.points[1].width = -1.0F;
    EXPECT_EQ(sp::validate(s).error().message, "points[1].width: expected a number in [0, 10000]");
    s.points[1].width = 1.0F;
    s.closed = true;
    EXPECT_EQ(sp::validate(s).error().message, "points: a closed polyline spline needs at least 3 points, got 2");
    sp::SplineKind k{};
    EXPECT_TRUE(sp::parse_kind("cubic_bezier", k));
    EXPECT_EQ(k, sp::SplineKind::cubic_bezier);
    EXPECT_FALSE(sp::parse_kind("bspline", k));
}

TEST(SplineArcLength, PolylineIsExactAndCurvesMatchBruteForce) {
    const sp::Curve line = build(make(sp::SplineKind::polyline, {{0, 0, 0}, {10, 0, 0}, {10, 0, 5}}));
    EXPECT_NEAR(line.length(), 15.0F, 2e-5F);
    EXPECT_NEAR(line.distance_at(1.5F), 12.5F, 2e-5F);
    EXPECT_NEAR(line.param_at(12.5F), 1.5F, 1e-5F);

    // A quarter circle as a cubic Bézier (handle 0.5522847498 r): its exact
    // length differs from pi r / 2 by the approximation, so compare to brute force.
    constexpr float k = 0.5522847498F * 10.0F;
    const sp::Curve quarter = build(make(sp::SplineKind::cubic_bezier, {{10, 0, 0}, {10, 0, k}, {k, 0, 10}, {0, 0, 10}}));
    EXPECT_NEAR(quarter.length(), static_cast<float>(brute_length(quarter)), 1e-4F);
    EXPECT_NEAR(quarter.length(), 15.7079632F, 0.01F);

    const sp::Curve wiggle = build(make(sp::SplineKind::centripetal_catmull_rom,
                                        {{0, 0, 0}, {2, 1, 3}, {4, -1, -2}, {7, 0, 5}, {8, 3, 0}, {12, 0, 1}}));
    EXPECT_NEAR(wiggle.length(), static_cast<float>(brute_length(wiggle)), 2e-4F);
}

TEST(SplineArcLength, ParamAndDistanceInvertEachOther) {
    const sp::Curve c = build(make(sp::SplineKind::centripetal_catmull_rom,
                                   {{0, 0, 0}, {5, 0, 1}, {6, 2, 8}, {-3, 0, 9}, {-4, 1, 2}}));
    for (int i = 0; i <= 64; ++i) {
        const float s = c.length() * static_cast<float>(i) / 64.0F;
        const float t = c.param_at(s);
        EXPECT_NEAR(c.distance_at(t), s, 1e-4F) << "s = " << s;
        EXPECT_NEAR(c.sample_at(s).distance, s, 0.0F);
    }
    EXPECT_EQ(c.param_at(-5.0F), 0.0F);
    EXPECT_EQ(c.param_at(1.0e9F), c.max_param());
}

TEST(SplineBounds, ExactExtremaOfTheBezier) {
    // y(u) = 30 u (1 - u): maximum 7.5 at u = 0.5.
    const sp::Curve arch = build(make(sp::SplineKind::cubic_bezier, {{0, 0, 0}, {0, 10, 0}, {10, 10, 0}, {10, 0, 0}}));
    EXPECT_EQ(arch.bounds().min, glm::vec3(0, 0, 0));
    EXPECT_NEAR(arch.bounds().max.y, 7.5F, 1e-6F);
    EXPECT_EQ(arch.bounds().max.x, 10.0F);

    sp::Spline wide = make(sp::SplineKind::polyline, {{0, 0, 0}, {10, 0, 0}});
    wide.points[0].width = 4.0F;
    wide.points[1].height = 3.0F;
    const sp::Bounds b = build(wide).swept_bounds();
    EXPECT_EQ(b.min, glm::vec3(-2, 0, -2));
    EXPECT_EQ(b.max, glm::vec3(12, 3, 2));
}

TEST(SplineSpacing, EvenSamplesFitToEndsAndClosedLaps) {
    const sp::Curve line = build(make(sp::SplineKind::polyline, {{0, 0, 0}, {10, 0, 0}}));
    auto s = sp::sample_by_spacing(line, {.spacing = 3.0F, .phase = 0.5F});
    ASSERT_EQ(s.size(), 4U); // 0.5, 3.5, 6.5, 9.5
    EXPECT_NEAR(s[3].position.x, 9.5F, 1e-5F);
    s = sp::sample_by_spacing(line, {.spacing = 3.0F, .phase = -1.0F});
    ASSERT_EQ(s.size(), 3U); // the pattern -1, 2, 5, 8: only 2, 5, 8 lie on the curve
    EXPECT_NEAR(s[0].position.x, 2.0F, 1e-5F);
    s = sp::sample_by_spacing(line, {.spacing = 3.0F, .fit_to_ends = true});
    ASSERT_EQ(s.size(), 4U); // 3 intervals of 10/3
    EXPECT_EQ(s[0].position.x, 0.0F);
    EXPECT_EQ(s[3].position.x, 10.0F);
    EXPECT_NEAR(s[1].position.x, 10.0F / 3.0F, 1e-5F);
    EXPECT_TRUE(sp::sample_by_spacing(line, {.spacing = 0.0F}).empty());
    EXPECT_EQ(sp::sample_by_spacing(line, {.spacing = 1.0e-6F, .max_samples = 10}).size(), 10U);

    const sp::Curve loop = build(make(sp::SplineKind::polyline, {{0, 0, 0}, {4, 0, 0}, {4, 0, 4}, {0, 0, 4}}, true));
    s = sp::sample_by_spacing(loop, {.spacing = 2.0F, .phase = 1.0F});
    ASSERT_EQ(s.size(), 8U);
    EXPECT_NEAR(s.back().distance, 15.0F, 1e-5F);
    s = sp::sample_by_spacing(loop, {.spacing = 2.0F, .phase = 15.0F});
    ASSERT_EQ(s.size(), 8U);
    EXPECT_NEAR(s[1].distance, 1.0F, 1e-5F); // wrapped past the seam
}

TEST(SplineAttributes, WidthHeightAndTagsFollowThePoints) {
    sp::Spline s = make(sp::SplineKind::polyline, {{0, 0, 0}, {10, 0, 0}, {20, 0, 0}});
    s.points[0].width = 2.0F;
    s.points[1].width = 4.0F;
    s.points[1].height = 1.0F;
    s.points[1].tags = 5;
    const sp::Curve c = build(s);
    EXPECT_NEAR(c.sample_at(5.0F).width, 3.0F, 1e-5F);
    EXPECT_NEAR(c.sample_at(5.0F).height, 0.5F, 1e-5F);
    EXPECT_EQ(c.sample_at(5.0F).tags, 0U);
    EXPECT_EQ(c.sample_at(15.0F).tags, 5U);
}

TEST(SplineTransform, AffineMapsMoveAndScaleWidths) {
    sp::Spline s = make(sp::SplineKind::polyline, {{0, 0, 0}, {1, 0, 0}});
    glm::mat4 m(2.0F);
    m[3] = glm::vec4(5, 1, 0, 1);
    const sp::Spline t = sp::transformed(s, m);
    EXPECT_EQ(t.points[1].position, glm::vec3(7, 1, 0));
    EXPECT_EQ(t.points[1].width, 2.0F);
}

// Bit-exact determinism: the same spline must give the same bits on every
// platform and compiler (CI runs this on Linux, Windows and macOS). Only
// + - * / sqrt in a fixed order, no contraction (CMakeLists.txt).
TEST(SplineDeterminism, SampleBitsMatchTheReference) {
    const sp::Curve c = build(make(sp::SplineKind::centripetal_catmull_rom,
                                   {{0, 0, 0}, {3.7F, 0.5F, 1.1F}, {6.2F, -0.25F, 4.9F}, {2.3F, 1.0F, 8.8F}}));
    const auto samples = sp::sample_by_spacing(c, {.spacing = 0.37F, .phase = 0.1F});
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](float f) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof bits);
        h = (h ^ bits) * 1099511628211ULL;
    };
    for (const sp::CurveSample& smp : samples) {
        mix(smp.position.x);
        mix(smp.position.y);
        mix(smp.position.z);
        mix(smp.tangent.x);
        mix(smp.param);
    }
    mix(c.length());
    EXPECT_EQ(samples.size(), 39U);
    EXPECT_EQ(h, 397902764304022192ULL) << "reference hash (update only for an intended algorithm change): " << h;
}

} // namespace
} // namespace quat::test
