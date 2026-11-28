// Stroke capture (ROADMAP G18.3): resampling, Ramer–Douglas–Peucker,
// Schneider's cubic fit within tolerance, loop closing, straight segments,
// corners, and capture driven by G5 input actions.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/input/action_map.hpp"
#include "engine/input/actions.hpp"
#include "engine/input/device_state.hpp"
#include "engine/spline/queries.hpp"
#include "engine/spline/stroke.hpp"

namespace quat::test {
namespace {

namespace sp = engine::spline;

constexpr float k_pi = 3.14159265F;

// Deterministic jitter in [-a, a] (splitmix64).
struct Jitter {
    std::uint64_t state = 7;
    float operator()(float a) {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        return a * (2.0F * static_cast<float>(z >> 40) / static_cast<float>(1ULL << 24) - 1.0F);
    }
};

[[nodiscard]] float max_gap(const sp::Curve& c, const std::vector<glm::vec3>& pts) {
    float worst = 0.0F;
    for (const glm::vec3& p : pts) worst = std::max(worst, sp::closest_point(c, p)->gap);
    return worst;
}

[[nodiscard]] sp::Curve curve_of(const sp::StrokeFit& fit) {
    auto c = sp::Curve::build(fit.spline);
    EXPECT_TRUE(c.has_value());
    return c ? *c : sp::Curve{};
}

TEST(SplineStroke, ResamplesByArcLength) {
    const std::vector<glm::vec3> line = {{0, 0, 0}, {3, 0, 0}, {3, 0, 4}};
    const auto r = sp::resample_polyline(line, 1.0F);
    ASSERT_EQ(r.size(), 8U); // 0..7 m
    EXPECT_EQ(r.front(), line.front());
    EXPECT_EQ(r.back(), line.back());
    EXPECT_NEAR(r[3].x, 3.0F, 1e-6F);
    EXPECT_NEAR(r[5].z, 2.0F, 1e-6F); // 5 m along: 2 m up the second leg
    for (std::size_t i = 1; i + 1 < r.size(); ++i) {
        EXPECT_NEAR(std::abs(r[i].x - r[i - 1].x) + std::abs(r[i].z - r[i - 1].z), 1.0F, 1e-5F);
    }
}

TEST(SplineStroke, RamerDouglasPeuckerDropsJitterKeepsCorners) {
    Jitter j;
    std::vector<glm::vec3> pts;
    for (int i = 0; i <= 40; ++i) pts.emplace_back(0.25F * static_cast<float>(i), 0.0F, j(0.04F));
    for (int i = 1; i <= 40; ++i) pts.emplace_back(10.0F, 0.0F, 0.25F * static_cast<float>(i) + j(0.04F));
    const auto keep = sp::simplify_rdp(pts, 0.05F);
    ASSERT_EQ(keep.size(), 3U);
    EXPECT_EQ(keep[0], 0U);
    EXPECT_EQ(keep[1], 40U); // the corner
    EXPECT_EQ(keep[2], pts.size() - 1);
}

TEST(SplineStroke, SchneiderFitStaysWithinTolerance) {
    // A half circle of radius 5, densely sampled.
    std::vector<glm::vec3> arc;
    for (int i = 0; i <= 100; ++i) {
        const float a = k_pi * static_cast<float>(i) / 100.0F;
        arc.emplace_back(5.0F * std::cos(a), 0.0F, 5.0F * std::sin(a));
    }
    for (const float tol : {0.1F, 0.01F}) {
        const auto segs = sp::fit_cubics(arc, tol);
        ASSERT_FALSE(segs.empty());
        const sp::Curve c = sp::Curve::from_segments(segs, false);
        EXPECT_LE(max_gap(c, arc), tol + 1e-4F) << "tolerance " << tol;
        EXPECT_EQ(c.position(0.0F), arc.front());
        EXPECT_EQ(c.position(c.max_param()), arc.back());
    }
    EXPECT_LE(sp::fit_cubics(arc, 0.1F).size(), 2U) << "a half circle needs at most two cubics at 10 cm";
    // Points on one cubic (chord-length parameters are not its own).
    const glm::vec3 b[4] = {{0, 0, 0}, {2, 0, 6}, {8, 0, 6}, {10, 0, 0}};
    std::vector<glm::vec3> on;
    for (int i = 0; i <= 50; ++i) on.push_back(sp::bezier_point(b, static_cast<float>(i) / 50.0F));
    const auto on_fit = sp::fit_cubics(on, 0.01F);
    EXPECT_LE(on_fit.size(), 3U);
    EXPECT_LE(max_gap(sp::Curve::from_segments(on_fit, false), on), 0.01F + 1e-4F);
    EXPECT_EQ(sp::fit_cubics(on, 0.2F).size(), 1U);
}

TEST(SplineStroke, NoisyLoopClosesAndStaysNearTheStroke) {
    Jitter j;
    std::vector<sp::StrokePoint> pts;
    std::vector<glm::vec3> raw;
    for (int i = 0; i <= 198; ++i) { // ends 3.6 degrees (0.5 m) short of the start
        const float a = 2.0F * k_pi * static_cast<float>(i) / 200.0F;
        const glm::vec3 p(8.0F * std::cos(a) + j(0.02F), 1.0F, 8.0F * std::sin(a) + j(0.02F));
        pts.push_back({p, false});
        raw.push_back(p);
    }
    const auto fit = sp::fit_stroke(pts);
    ASSERT_TRUE(fit.has_value()) << fit.error().message;
    EXPECT_TRUE(fit->closed);
    EXPECT_TRUE(fit->spline.closed);
    EXPECT_EQ(fit->spline.kind, sp::SplineKind::centripetal_catmull_rom);
    const sp::Curve c = curve_of(*fit);
    // Every hit (except the dropped tail near the seam) is near the curve.
    raw.resize(190);
    // The Bézier fit is within fit_tolerance of the de-jittered stroke, the
    // Catmull-Rom points within fit_tolerance of the fit, the de-jittering
    // within jitter_tolerance of the hits, which carry +-0.02 of noise.
    EXPECT_LE(max_gap(c, raw), 0.1F + 0.1F + 0.05F + 0.02F);
    EXPECT_NEAR(c.length(), 2.0F * k_pi * 8.0F, 0.5F);
    EXPECT_LT(fit->spline.points.size(), 40U) << "the fit is compact";

    // Bézier output: the fitted pieces themselves.
    sp::StrokeOptions o;
    o.output = sp::StrokeOutput::cubic_bezier;
    const auto bez = sp::fit_stroke(pts, o);
    ASSERT_TRUE(bez.has_value());
    EXPECT_EQ(bez->spline.kind, sp::SplineKind::cubic_bezier);
    EXPECT_EQ(bez->spline.points.size(), 3 * bez->beziers.size());
}

TEST(SplineStroke, StraightSegmentsStayStraightAndCornersSharp) {
    std::vector<sp::StrokePoint> pts;
    pts.push_back({{0, 0, 0}, false});
    pts.push_back({{10, 0, 0}, true}); // drawn with the straight modifier
    for (int i = 1; i <= 40; ++i) {     // then freehand, turning a corner up +Z
        pts.push_back({{10.0F, 0.0F, 0.25F * static_cast<float>(i)}, false});
    }
    sp::StrokeOptions o;
    o.output = sp::StrokeOutput::cubic_bezier;
    const auto bez = sp::fit_stroke(pts, o);
    ASSERT_TRUE(bez.has_value()) << bez.error().message;
    ASSERT_GE(bez->beziers.size(), 2U);
    for (int k = 0; k < 4; ++k) EXPECT_EQ(bez->beziers[0].p[k].z, 0.0F) << "exactly straight";

    // Catmull-Rom output (the default): doubled knots at the corner keep the
    // straight run exactly straight and the corner exactly sharp.
    const auto cr = sp::fit_stroke(pts);
    ASSERT_TRUE(cr.has_value());
    const sp::Curve c = curve_of(*cr);
    const float corner = sp::closest_point(c, {10, 0, 0})->distance;
    EXPECT_NEAR(corner, 10.0F, 1e-4F);
    for (int i = 0; i <= 200; ++i) {
        const glm::vec3 p = c.sample_at(corner * static_cast<float>(i) / 200.0F).position;
        EXPECT_EQ(p.z, 0.0F) << "the straight run bows at x = " << p.x;
        EXPECT_EQ(p.y, 0.0F);
    }
    EXPECT_LE(sp::closest_point(c, {10, 0, 0})->gap, 1e-6F) << "the corner stays sharp";
    // Past the corner the freehand run is straight too (it was drawn straight).
    for (int i = 1; i <= 50; ++i) {
        const glm::vec3 p = c.sample_at(corner + (c.length() - corner) * static_cast<float>(i) / 50.0F).position;
        EXPECT_NEAR(p.x, 10.0F, 1e-4F);
    }
}

TEST(SplineStroke, HugeStrokesAreBoundedNotHung) {
    // 2e6 m at 1 cm would be 2e8 samples, and a float accumulator stalls
    // past ~2.6e5 m: resampling is capped and walks by integer index.
    const std::vector<glm::vec3> line = {{-1.0e6F, 0, 0}, {1.0e6F, 0, 0}};
    const auto r = sp::resample_polyline(line, 0.01F, 1000);
    EXPECT_LE(r.size(), 1000U);
    EXPECT_EQ(r.front(), line.front());
    EXPECT_EQ(r.back(), line.back());
    for (std::size_t i = 1; i < r.size(); ++i) EXPECT_GT(r[i].x, r[i - 1].x);
    // 3e5 m at 0.02 m: below the cap, and past where s += spacing stalls.
    const std::vector<glm::vec3> far = {{0, 0, 0}, {3.0e5F, 0, 0}};
    const auto f = sp::resample_polyline(far, 0.5F);
    EXPECT_EQ(f.size(), 600001U);
    EXPECT_EQ(f.back().x, 3.0e5F);

    std::vector<sp::StrokePoint> huge = {{{-1.0e6F, 0, 0}, false}, {{1.0e6F, 0, 0}, false}};
    sp::StrokeOptions o;
    o.sample_spacing = 0.01F;
    const auto fit = sp::fit_stroke(huge, o);
    ASSERT_FALSE(fit.has_value());
    EXPECT_EQ(fit.error().message.rfind("sample_spacing: use >= ", 0), 0U) << fit.error().message;
}

TEST(SplineStroke, RefusesBadStrokes) {
    std::vector<sp::StrokePoint> one = {{{0, 0, 0}, false}};
    EXPECT_EQ(sp::fit_stroke(one).error().message, "points: a stroke needs at least 2 points, got 1");
    std::vector<sp::StrokePoint> tiny = {{{0, 0, 0}, false}, {{0.1F, 0, 0}, false}};
    EXPECT_NE(sp::fit_stroke(tiny).error().message.find("shorter than min_length"), std::string::npos);
    sp::StrokeOptions o;
    o.fit_tolerance = 0.0F;
    EXPECT_EQ(sp::fit_stroke(tiny, o).error().message, "fit_tolerance: expected a number > 0");
}

TEST(SplineStroke, CaptureFollowsTheDrawActionAndStraightModifier) {
    auto map = engine::input::parse_action_map(nlohmann::json::parse(R"({
        "version": 1,
        "contexts": [{"name": "build", "active": true}],
        "actions": [
          {"name": "stroke.draw", "type": "button", "context": "build", "trigger": {"drag": 4},
           "bindings": ["mouse:left"]},
          {"name": "stroke.straight", "type": "button", "context": "build", "bindings": ["key:left_shift"]}
        ]})"));
    ASSERT_TRUE(map.has_value()) << map.error().message;
    engine::input::Actions actions(*map);
    engine::input::DeviceState d;
    // The ground: the plane y = 0, one metre per 10 pixels.
    const sp::PointerGroundFn ground = [](glm::vec2 px) -> std::optional<glm::vec3> {
        return glm::vec3(px.x / 10.0F, 0.0F, px.y / 10.0F);
    };
    sp::StrokeCapture cap;
    const sp::StrokeBindings bind;
    auto step = [&](glm::vec2 pointer, bool button, bool shift) {
        d.begin_frame();
        d.mouse.reset();
        d.keys.reset();
        d.mouse_position = pointer;
        if (button) d.mouse.set(0);
        if (shift) d.keys.set(engine::input::key_from_name("left_shift").value());
        actions.update(d, 1.0F / 60.0F);
        return cap.update(actions, bind, ground);
    };
    EXPECT_EQ(step({0, 0}, true, false), sp::StrokeEvent::none) << "a press is not a drag yet";
    EXPECT_EQ(step({10, 0}, true, false), sp::StrokeEvent::began);
    EXPECT_EQ(cap.points().front().position, glm::vec3(0, 0, 0)) << "starts at the press point";
    for (int x = 20; x <= 100; x += 10) EXPECT_EQ(step({static_cast<float>(x), 0}, true, false), sp::StrokeEvent::moved);
    // Shift: a straight segment that follows the pointer.
    step({100, 50}, true, true);
    step({100, 100}, true, true);
    EXPECT_TRUE(cap.points().back().straight);
    EXPECT_EQ(cap.points().back().position, glm::vec3(10, 0, 10));
    EXPECT_EQ(step({100, 100}, false, false), sp::StrokeEvent::finished);
    auto result = cap.take_result();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value()) << result->error().message;
    EXPECT_FALSE((*result)->closed);
    EXPECT_FALSE(cap.take_result().has_value()) << "taken once";
    EXPECT_FALSE(cap.active());
}

} // namespace
} // namespace quat::test
