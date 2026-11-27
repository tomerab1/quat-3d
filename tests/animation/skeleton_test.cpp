// Skeleton runtime: bind-pose composition through the hierarchy, inverse bind.

#include <vector>

#include <gtest/gtest.h>

#include "engine/animation/skeleton.hpp"
#include "support/glm_near.hpp"

namespace {

using engine::animation::SkeletonAsset;

// Root at the origin; child translated +2 on X, parented under the root.
[[nodiscard]] SkeletonAsset root_and_child() {
    SkeletonAsset s;
    s.joints.resize(2);
    s.joints[0].name = "root";
    s.joints[0].parent = -1;
    s.joints[1].name = "child";
    s.joints[1].parent = 0;
    s.joints[1].translation = {2.0F, 0.0F, 0.0F};
    return s;
}

TEST(AnimSkeleton, BindWorldComposesThroughTheHierarchy) {
    const std::vector<glm::mat4> world = engine::animation::compute_bind_world(root_and_child());
    ASSERT_EQ(world.size(), 2U);
    EXPECT_VEC_NEAR(glm::vec3(world[1][3]), glm::vec3(2.0F, 0.0F, 0.0F), 1e-5F);
}

// With inverse_bind = inverse(world_bind) the skinning matrix in bind pose is
// identity (a bind-pose vertex is unmoved) — the defining property.
TEST(AnimSkeleton, InverseBindSkinsTheBindPoseToIdentity) {
    SkeletonAsset s = root_and_child();
    const std::vector<glm::mat4> world = engine::animation::compute_bind_world(s);
    ASSERT_EQ(world.size(), s.joints.size());
    for (std::size_t i = 0; i < s.joints.size(); ++i) {
        s.joints[i].inverse_bind = glm::inverse(world[i]);
    }
    const glm::mat4 identity(1.0F);
    for (std::size_t i = 0; i < s.joints.size(); ++i) {
        const glm::mat4 skinning = world[i] * s.joints[i].inverse_bind;
        for (int c = 0; c < 4; ++c) {
            EXPECT_VEC_NEAR(skinning[c], identity[c], 1e-5F) << "joint " << i << ", column " << c;
        }
    }
}

} // namespace
