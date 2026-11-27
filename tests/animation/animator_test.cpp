// Animator: single-clip posing, 1D blend, masked additive layers.

#include <vector>

#include <gtest/gtest.h>

#include "animation/test_rig.hpp"
#include "engine/animation/animator.hpp"
#include "support/glm_near.hpp"

namespace {

using namespace engine::animation;
using quat::test::displacement;
using quat::test::k_no_translation;

// Child (node 1) translation animated from its bind (2,0,0) to (5,0,0).
[[nodiscard]] AnimClipAsset child_slide_clip() {
    AnimClipAsset clip;
    clip.duration = 1.0F;
    AnimSampler s;
    s.input = {0.0F, 1.0F};
    s.output = {{2, 0, 0, 0}, {5, 0, 0, 0}};
    clip.samplers = {s};
    clip.channels = {AnimChannel{1, AnimPath::translation, 0}};
    return clip;
}

TEST(AnimPose, ClipAtBindTimeSkinsToIdentity) {
    const SkeletonAsset skel = quat::test::two_joint_skeleton();
    const std::vector<glm::mat4> m = sample_skinning_matrices(skel, child_slide_clip(), 0.0F);
    ASSERT_EQ(m.size(), 2U);
    for (std::size_t i = 0; i < m.size(); ++i) {
        EXPECT_VEC_NEAR(m[i][3], k_no_translation, 1e-5F) << "joint " << i;
    }
}

TEST(AnimPose, PosedChildIsDisplacedAndUnanimatedRootIsNot) {
    const SkeletonAsset skel = quat::test::two_joint_skeleton();
    const std::vector<glm::mat4> m = sample_skinning_matrices(skel, child_slide_clip(), 1.0F);
    ASSERT_EQ(m.size(), 2U);
    EXPECT_VEC_NEAR(displacement(m[1]), glm::vec3(3.0F, 0.0F, 0.0F), 1e-5F);
    EXPECT_VEC_NEAR(m[0][3], k_no_translation, 1e-5F);
}

// clip_a holds the child at bind (2,0,0), clip_b at (8,0,0): at blend 0.5 the
// child sits at (5,0,0), so skinning displaces a bind vertex by (3,0,0).
TEST(AnimBlend, LinearBlendMidpoint) {
    const SkeletonAsset skel = quat::test::two_joint_skeleton();
    const AnimClipAsset a = quat::test::child_translation_clip({2.0F, 0.0F, 0.0F});
    const AnimClipAsset b = quat::test::child_translation_clip({8.0F, 0.0F, 0.0F});

    BlendTree tree;
    tree.clip_a = &a;
    tree.clip_b = &b;
    tree.blend = 0.5F;
    const std::vector<glm::mat4> m = tree.evaluate(skel);
    ASSERT_EQ(m.size(), 2U);
    EXPECT_VEC_NEAR(displacement(m[1]), glm::vec3(3.0F, 0.0F, 0.0F), 1e-4F);
}

class AnimAdditive : public ::testing::Test {
protected:
    // Base = clip_a (child at bind). The additive clip moves the child to
    // (3,0,0): a delta of (1,0,0) from bind.
    SkeletonAsset skel = quat::test::two_joint_skeleton();
    AnimClipAsset base = quat::test::child_translation_clip({2.0F, 0.0F, 0.0F});
    AnimClipAsset add = quat::test::child_translation_clip({3.0F, 0.0F, 0.0F});

    [[nodiscard]] std::vector<glm::mat4> evaluate(std::uint32_t mask) const {
        BlendTree tree;
        tree.clip_a = &base;
        tree.additive = &add;
        tree.additive_weight = 1.0F;
        tree.additive_mask = {mask};
        return tree.evaluate(skel);
    }
};

TEST_F(AnimAdditive, LayerAppliesItsDeltaToEnabledJoints) {
    const std::vector<glm::mat4> m = evaluate(0x2U); // only joint 1 (the child)
    ASSERT_EQ(m.size(), 2U);
    EXPECT_VEC_NEAR(displacement(m[1]), glm::vec3(1.0F, 0.0F, 0.0F), 1e-4F);
    EXPECT_VEC_NEAR(m[0][3], k_no_translation, 1e-4F) << "additive leaked to the masked-out root";
}

TEST_F(AnimAdditive, MaskedOutJointStaysAtBind) {
    const std::vector<glm::mat4> m = evaluate(0x1U); // only the root
    ASSERT_EQ(m.size(), 2U);
    EXPECT_VEC_NEAR(m[1][3], k_no_translation, 1e-4F) << "masked-out child was still affected";
}

} // namespace
