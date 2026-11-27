// Animation state machine: parameter-driven transitions with a crossfade.

#include <vector>

#include <gtest/gtest.h>

#include "animation/test_rig.hpp"
#include "engine/animation/state_machine.hpp"
#include "support/glm_near.hpp"

namespace {

using namespace engine::animation;
using quat::test::displacement;

// idle holds the child at bind (2,0,0), walk at (5,0,0); idle -> walk when
// "speed" > 0.5, over a 1 s crossfade.
class AnimStateMachineTest : public ::testing::Test {
protected:
    SkeletonAsset    skel = quat::test::two_joint_skeleton();
    AnimClipAsset    idle = quat::test::child_translation_clip({2.0F, 0.0F, 0.0F});
    AnimClipAsset    walk = quat::test::child_translation_clip({5.0F, 0.0F, 0.0F});
    AnimStateMachine sm;
    int              idle_state = sm.add_state({"idle", &idle, 1.0F, true});
    int              walk_state = sm.add_state({"walk", &walk, 1.0F, true});

    void SetUp() override {
        sm.add_transition(
            {idle_state, walk_state, {"speed", AnimCondition::Op::greater, 0.5F}, 1.0F});
        sm.set_current(idle_state);
    }
};

TEST_F(AnimStateMachineTest, StaysIdleBelowTheThreshold) {
    const std::vector<glm::mat4> m = sm.update(skel, 0.0F);
    EXPECT_EQ(sm.current_state(), idle_state);
    EXPECT_FALSE(sm.transitioning());
    ASSERT_EQ(m.size(), 2U);
    EXPECT_VEC_NEAR(m[1][3], quat::test::k_no_translation, 1e-4F) << "idle pose is not bind";
}

TEST_F(AnimStateMachineTest, CrossfadesToWalkAndCompletes) {
    ASSERT_EQ(sm.update(skel, 0.0F).size(), 2U);

    // Raising the parameter starts the transition (weight 0: still the idle pose).
    sm.set_parameter("speed", 1.0F);
    (void)sm.update(skel, 0.0F);
    ASSERT_TRUE(sm.transitioning()) << "transition did not start";

    // Halfway through the 1 s crossfade the child blends idle(2) -> walk(5) to
    // 3.5, so skinning displaces a bind vertex by (1.5,0,0).
    std::vector<glm::mat4> m = sm.update(skel, 0.5F);
    ASSERT_EQ(m.size(), 2U);
    EXPECT_VEC_NEAR(displacement(m[1]), glm::vec3(1.5F, 0.0F, 0.0F), 1e-3F)
        << "mid-crossfade pose";

    // Completing the crossfade lands on walk: child at (5,0,0), displacement (3,0,0).
    m = sm.update(skel, 0.5F);
    EXPECT_FALSE(sm.transitioning()) << "transition did not complete";
    EXPECT_EQ(sm.current_state(), walk_state);
    ASSERT_EQ(m.size(), 2U);
    EXPECT_VEC_NEAR(displacement(m[1]), glm::vec3(3.0F, 0.0F, 0.0F), 1e-3F) << "final walk pose";
}

} // namespace
