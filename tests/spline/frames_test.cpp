// Curve frames (ROADMAP G18.1): up-locked frames stay vertical, rotation-
// minimising frames (Wang et al. 2008) do not twist about the tangent and do
// not flip on helices, and ground draping follows the slope.

#include <cmath>
#include <optional>

#include <gtest/gtest.h>

#include "engine/spline/frames.hpp"
#include "engine/spline/ground.hpp"

namespace quat::test {
namespace {

namespace sp = engine::spline;

[[nodiscard]] sp::Curve curve_of(sp::SplineKind kind, std::vector<glm::vec3> pts, bool closed = false) {
    sp::Spline s;
    s.kind = kind;
    s.closed = closed;
    for (const glm::vec3& p : pts) s.points.push_back({p, 1.0F, 0.0F, 0});
    auto c = sp::Curve::build(s);
    EXPECT_TRUE(c.has_value());
    return c ? *c : sp::Curve{};
}

void expect_orthonormal(const sp::Frame& f) {
    EXPECT_NEAR(glm::length(f.tangent), 1.0F, 1e-5F);
    EXPECT_NEAR(glm::length(f.up), 1.0F, 1e-5F);
    EXPECT_NEAR(glm::length(f.right), 1.0F, 1e-5F);
    EXPECT_NEAR(glm::dot(f.tangent, f.up), 0.0F, 1e-5F);
    EXPECT_NEAR(glm::dot(f.tangent, f.right), 0.0F, 1e-5F);
    EXPECT_NEAR(glm::dot(f.up, f.right), 0.0F, 1e-5F);
}

// A helix of radius 5 and pitch 4 per turn, three turns, as a dense polyline
// through a Catmull-Rom spline.
[[nodiscard]] sp::Curve helix() {
    std::vector<glm::vec3> pts;
    for (int i = 0; i <= 72; ++i) {
        const float a = static_cast<float>(i) * (3.14159265F / 12.0F);
        pts.emplace_back(5.0F * std::cos(a), 4.0F * static_cast<float>(i) / 24.0F, 5.0F * std::sin(a));
    }
    return curve_of(sp::SplineKind::centripetal_catmull_rom, pts);
}

TEST(SplineFrames, UpLockedFramesStayVerticalOnSlopes) {
    const sp::Curve ramp = curve_of(sp::SplineKind::polyline, {{0, 0, 0}, {10, 5, 0}, {10, 5, 10}});
    const auto f = sp::sample_frames(ramp, {.spacing = 1.0F}, sp::FrameMode::up_locked);
    ASSERT_FALSE(f.empty());
    EXPECT_EQ(f[2].up, glm::vec3(0, 1, 0));
    EXPECT_NEAR(f[2].tangent.x, 1.0F, 1e-6F);
    EXPECT_EQ(f[2].tangent.y, 0.0F);
    EXPECT_NEAR(f[2].right.z, 1.0F, 1e-6F); // right of travel along +X is +Z
    EXPECT_NEAR(f.back().right.x, -1.0F, 1e-6F);
    for (const sp::Frame& fr : f) expect_orthonormal(fr);
    // Frame matrix: X tangent, Y up, Z right, translation the position.
    const glm::mat4 m = f[2].matrix();
    EXPECT_EQ(glm::vec3(m[3]), f[2].position);
    EXPECT_EQ(glm::vec3(m[1]), f[2].up);
}

TEST(SplineFrames, VerticalTangentGetsADeterministicHeading) {
    sp::CurveSample s;
    s.tangent = glm::vec3(0, 1, 0);
    const sp::Frame f = sp::up_locked_frame(s);
    EXPECT_EQ(f.tangent, glm::vec3(1, 0, 0));
    expect_orthonormal(f);
}

TEST(SplineFrames, RotationMinimisingFramesOnAFlatCurveKeepWorldUp) {
    const sp::Curve wiggle = curve_of(sp::SplineKind::centripetal_catmull_rom,
                                      {{0, 0, 0}, {4, 0, 3}, {8, 0, -2}, {12, 0, 4}, {16, 0, 0}});
    const auto f = sp::sample_frames(wiggle, {.spacing = 0.1F}, sp::FrameMode::rotation_minimising);
    for (const sp::Frame& fr : f) {
        EXPECT_NEAR(fr.up.y, 1.0F, 1e-5F);
        expect_orthonormal(fr);
    }
}

TEST(SplineFrames, RotationMinimisingFramesDoNotTwistOrFlipOnAHelix) {
    const sp::Curve c = helix();
    const auto f = sp::sample_frames(c, {.spacing = 0.05F}, sp::FrameMode::rotation_minimising);
    ASSERT_GT(f.size(), 1000U);
    float worst_twist = 0.0F;
    for (std::size_t i = 0; i + 1 < f.size(); ++i) {
        expect_orthonormal(f[i]);
        // No flip: consecutive frames stay close.
        ASSERT_GT(glm::dot(f[i].up, f[i + 1].up), 0.99F) << "flip at sample " << i;
        // Rotation-minimising: d(up)/ds has no component along right (the
        // frame's angular velocity about the tangent is zero). The Frenet
        // frame of this helix twists at its torsion b / (a^2 + b^2) ~ 0.025 rad/m.
        const float ds = f[i + 1].distance - f[i].distance;
        const float twist = glm::dot(f[i + 1].up - f[i].up, f[i].right) / ds;
        worst_twist = std::max(worst_twist, std::abs(twist));
    }
    EXPECT_LT(worst_twist, 2e-3F);
}

TEST(SplineFrames, ModeNamesRoundTrip) {
    sp::FrameMode m{};
    EXPECT_TRUE(sp::parse_frame_mode("rotation_minimising", m));
    EXPECT_EQ(m, sp::FrameMode::rotation_minimising);
    EXPECT_TRUE(sp::parse_frame_mode(sp::frame_mode_name(sp::FrameMode::up_locked), m));
    EXPECT_EQ(m, sp::FrameMode::up_locked);
    EXPECT_FALSE(sp::parse_frame_mode("frenet", m));
}

TEST(SplineGround, DrapesOntoTheGroundAndTiltsTangents) {
    const sp::Curve line = curve_of(sp::SplineKind::polyline, {{0, 3, 0}, {10, 3, 0}});
    auto samples = sp::sample_by_spacing(line, {.spacing = 1.0F});
    // Ground y = x / 2, with a hole (no tile loaded) beyond x = 8.
    const sp::GroundHeightFn ground = [](float x, float) -> std::optional<float> {
        if (x > 8.5F) return std::nullopt;
        return x * 0.5F;
    };
    EXPECT_EQ(sp::drape(samples, ground, 0.25F), 2U);
    EXPECT_NEAR(samples[4].position.y, 2.25F, 1e-6F);
    EXPECT_EQ(samples[10].position.y, 3.0F); // kept
    const glm::vec3 slope = glm::normalize(glm::vec3(1.0F, 0.5F, 0.0F));
    EXPECT_NEAR(samples[4].tangent.x, slope.x, 1e-5F);
    EXPECT_NEAR(samples[4].tangent.y, slope.y, 1e-5F);

    sp::Spline s;
    s.points = {{{2, 9, 0}, 1, 0, 0}, {{20, 9, 0}, 1, 0, 0}};
    EXPECT_EQ(sp::project_points(s, ground, 0.0F), 1U);
    EXPECT_EQ(s.points[0].position.y, 1.0F);
    EXPECT_EQ(sp::project_points(s, {}, 0.0F), 2U);
}

} // namespace
} // namespace quat::test
