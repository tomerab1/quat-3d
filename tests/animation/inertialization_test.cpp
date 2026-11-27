// Inertialization: Bollo's quintic and inertialized state-machine transitions.

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "animation/test_rig.hpp"
#include "engine/animation/inertialization.hpp"
#include "engine/animation/state_machine.hpp"
#include "support/glm_near.hpp"

namespace {

using namespace engine::animation;
using quat::test::displacement;

constexpr float k_dt = 1.0F / 60.0F;

// Child (node 1) translation keyed linearly from `from` to `to` over 1 s.
[[nodiscard]] AnimClipAsset child_slide(glm::vec3 from, glm::vec3 to) {
    AnimClipAsset clip;
    clip.duration = 1.0F;
    AnimSampler s;
    s.input = {0.0F, 1.0F};
    s.output = {glm::vec4(from, 0.0F), glm::vec4(to, 0.0F)};
    clip.samplers = {s};
    clip.channels = {AnimChannel{1, AnimPath::translation, 0}};
    return clip;
}

// Root (node 0) rotation about +Z keyed from `from` to `to` radians over 1 s.
[[nodiscard]] AnimClipAsset root_spin(float from, float to) {
    const auto key = [](float angle) {
        const glm::quat q = glm::angleAxis(angle, glm::vec3(0.0F, 0.0F, 1.0F));
        return glm::vec4(q.x, q.y, q.z, q.w); // glTF order
    };
    AnimClipAsset clip;
    clip.duration = 1.0F;
    AnimSampler s;
    s.input = {0.0F, 0.5F, 1.0F};
    s.output = {key(from), key(0.5F * (from + to)), key(to)};
    clip.samplers = {s};
    clip.channels = {AnimChannel{0, AnimPath::rotation, 0}};
    return clip;
}

// The child's skinned displacement along X (bind child at x=2).
[[nodiscard]] float child_x(const std::vector<glm::mat4>& m) { return displacement(m[1]).x + 2.0F; }

// Rotation angle about +Z of the root's skinning matrix.
[[nodiscard]] float root_angle(const std::vector<glm::mat4>& m) {
    return std::atan2(m[0][0][1], m[0][0][0]);
}

TEST(InertialCurve, StartsAtOffsetWithItsVelocityAndSettlesAtZero) {
    const InertialCurve c = InertialCurve::make(0.4F, -0.5F, 0.3F);
    EXPECT_FLOAT_EQ(c.evaluate(0.0F), 0.4F);
    const float h = 1e-4F;
    EXPECT_NEAR((c.evaluate(h) - c.evaluate(0.0F)) / h, -0.5F, 0.02F) << "initial velocity";
    EXPECT_EQ(c.evaluate(0.3F), 0.0F);
    // Zero velocity at the end: the last stretch is flat.
    EXPECT_NEAR(c.evaluate(0.3F - h), 0.0F, 1e-6F);
    for (float t = 0.0F; t < 0.3F; t += 0.005F) {
        EXPECT_GE(c.evaluate(t), -1e-5F) << "overshoot at t=" << t;
    }
}

TEST(InertialCurve, FastApproachShortensTheBlendInsteadOfOvershooting) {
    // At -10 m/s a 0.2 offset would cross zero in 0.02 s; the duration
    // clamps to -5 x0 / v0 = 0.1 s.
    const InertialCurve c = InertialCurve::make(0.2F, -10.0F, 0.5F);
    EXPECT_NEAR(c.t1, 0.1F, 1e-6F);
    for (float t = 0.0F; t <= 0.1F; t += 0.001F) EXPECT_GE(c.evaluate(t), -1e-5F);
}

TEST(Inertializer, AtTheStartTheOutputIsTheSourcePose) {
    std::vector<JointPose> source(1);
    source[0].translation = {1.0F, 2.0F, 3.0F};
    source[0].rotation = glm::angleAxis(0.7F, glm::normalize(glm::vec3(1.0F, 1.0F, 0.0F)));
    source[0].scale = glm::vec3(1.5F);
    const std::vector<JointPose> target(1); // identity
    Inertializer in;
    in.begin(source, source, k_dt, target, target, k_dt, 0.25F);
    ASSERT_TRUE(in.active());

    std::vector<JointPose> pose = target;
    in.apply(pose);
    EXPECT_VEC_NEAR(pose[0].translation, source[0].translation, 1e-5F);
    EXPECT_SAME_ROTATION(pose[0].rotation, source[0].rotation, 1e-5F);
    EXPECT_VEC_NEAR(pose[0].scale, source[0].scale, 1e-5F);

    in.advance(0.25F);
    EXPECT_FALSE(in.active());
}

// slide moves the child +2 m/s along X; hold keeps it at x = 2.5. The switch
// fires at t = 0.5 s (child at x = 3), 0.5 s blend.
class InertializedTransition : public ::testing::Test {
protected:
    SkeletonAsset    skel = quat::test::two_joint_skeleton();
    AnimClipAsset    slide = child_slide({2.0F, 0.0F, 0.0F}, {4.0F, 0.0F, 0.0F});
    AnimClipAsset    hold = quat::test::child_translation_clip({2.5F, 0.0F, 0.0F});
    AnimStateMachine sm;
    int              slide_state = sm.add_state({"slide", &slide, 1.0F, false});
    int              hold_state = sm.add_state({"hold", &hold, 1.0F, true});

    void SetUp() override {
        sm.add_transition({slide_state, hold_state, {"go", AnimCondition::Op::greater, 0.5F}, 0.5F,
                           BlendMode::inertialize});
        sm.set_current(slide_state);
    }

    // Plays slide up to t = 0.5 s and returns the child x of every frame.
    std::vector<float> play_slide() {
        std::vector<float> xs;
        xs.push_back(child_x(sm.update(skel, 0.0F)));
        for (int i = 0; i < 30; ++i) xs.push_back(child_x(sm.update(skel, k_dt)));
        return xs;
    }
};

TEST_F(InertializedTransition, PoseAndVelocityAreContinuousAcrossTheSwitch) {
    std::vector<float> xs = play_slide();
    ASSERT_NEAR(xs.back(), 3.0F, 1e-4F);

    sm.set_parameter("go", 1.0F);
    xs.push_back(child_x(sm.update(skel, k_dt)));
    ASSERT_EQ(sm.current_state(), hold_state);
    ASSERT_TRUE(sm.transitioning());
    xs.push_back(child_x(sm.update(skel, k_dt)));

    const std::size_t n = xs.size();
    const float v_before = (xs[n - 3] - xs[n - 4]) / k_dt;
    const float v_switch = (xs[n - 2] - xs[n - 3]) / k_dt;
    const float v_after = (xs[n - 1] - xs[n - 2]) / k_dt;
    EXPECT_NEAR(v_before, 2.0F, 1e-2F);
    EXPECT_NEAR(v_switch, v_before, 0.1F) << "velocity jumps at the switch";
    EXPECT_NEAR(v_after, v_switch, 0.2F) << "velocity jumps after the switch";
    // A cut would jump the child 0.5 m back to the hold pose in one frame.
    EXPECT_LT(std::abs(xs[n - 2] - xs[n - 3]), 0.05F);
}

TEST_F(InertializedTransition, ConvergesToTheTargetByTheBlendTimeEvaluatingOneClip) {
    (void)play_slide();
    sm.set_parameter("go", 1.0F);
    (void)sm.update(skel, k_dt);
    EXPECT_EQ(sm.evaluations(), 2) << "the switch frame samples the target twice (velocity)";

    for (int i = 0; i < 28; ++i) {
        (void)sm.update(skel, k_dt);
        EXPECT_EQ(sm.evaluations(), 1) << "frame " << i << " evaluated more than the target";
        EXPECT_TRUE(sm.transitioning());
    }
    (void)sm.update(skel, k_dt);
    const float x = child_x(sm.update(skel, k_dt)); // 31 frames > the 0.5 s blend
    EXPECT_EQ(sm.evaluations(), 1);
    EXPECT_FALSE(sm.transitioning());
    EXPECT_NEAR(x, 2.5F, 1e-5F) << "not on the target pose after the blend";
}

TEST_F(InertializedTransition, CrossfadeEvaluatesBothClips) {
    AnimStateMachine fade;
    const int a = fade.add_state({"slide", &slide, 1.0F, false});
    const int b = fade.add_state({"hold", &hold, 1.0F, true});
    fade.add_transition({a, b, {"go", AnimCondition::Op::greater, 0.5F}, 0.5F,
                         BlendMode::crossfade});
    fade.set_current(a);
    (void)fade.update(skel, k_dt);
    fade.set_parameter("go", 1.0F);
    (void)fade.update(skel, k_dt);
    (void)fade.update(skel, k_dt);
    EXPECT_EQ(fade.evaluations(), 2);
}

TEST(InertializedRotation, AngularVelocityIsContinuousAndSettles) {
    const SkeletonAsset skel = quat::test::two_joint_skeleton();
    const AnimClipAsset spin = root_spin(0.0F, 1.0F); // +1 rad/s
    const AnimClipAsset still = root_spin(0.2F, 0.2F);
    AnimStateMachine sm;
    const int a = sm.add_state({"spin", &spin, 1.0F, false});
    const int b = sm.add_state({"still", &still, 1.0F, true});
    sm.add_transition({a, b, {"go", AnimCondition::Op::greater, 0.5F}, 0.4F,
                       BlendMode::inertialize});
    sm.set_current(a);

    std::vector<float> angles;
    angles.push_back(root_angle(sm.update(skel, 0.0F)));
    for (int i = 0; i < 30; ++i) angles.push_back(root_angle(sm.update(skel, k_dt)));
    sm.set_parameter("go", 1.0F);
    angles.push_back(root_angle(sm.update(skel, k_dt)));

    const std::size_t n = angles.size();
    const float w_before = (angles[n - 2] - angles[n - 3]) / k_dt;
    const float w_after = (angles[n - 1] - angles[n - 2]) / k_dt;
    EXPECT_NEAR(w_before, 1.0F, 0.02F);
    EXPECT_NEAR(w_after, w_before, 0.1F) << "angular velocity jumps at the switch";

    float angle = 0.0F;
    for (int i = 0; i < 24; ++i) angle = root_angle(sm.update(skel, k_dt));
    EXPECT_FALSE(sm.transitioning());
    EXPECT_NEAR(angle, 0.2F, 1e-5F);
}

} // namespace
