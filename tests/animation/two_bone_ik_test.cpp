// Two-bone IK: exact reach, clamped reach, pole side, and the local-pose rewrite.

#include <cmath>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/animation/two_bone_ik.hpp"
#include "support/glm_near.hpp"

namespace {

using namespace engine::animation;

// Hip at the origin, knee bent forward (+Z), ankle straight below the hip.
const glm::vec3 k_hip(0.0F, 0.0F, 0.0F);
const glm::vec3 k_knee(0.0F, -0.5F, 0.1F);
const glm::vec3 k_ankle(0.0F, -1.0F, 0.0F);

// Signed distance of `p` from the hip-target line toward `side`.
[[nodiscard]] float side_of_line(const glm::vec3& p, const glm::vec3& target,
                                 const glm::vec3& side) {
    const glm::vec3 dir = glm::normalize(target - k_hip);
    const glm::vec3 off = p - k_hip - glm::dot(p - k_hip, dir) * dir;
    return glm::dot(off, side);
}

TEST(TwoBoneIk, ReachesAReachableTargetExactly) {
    const glm::vec3 target(0.3F, -0.7F, 0.2F);
    const TwoBoneIkSolution s =
        solve_two_bone_ik(k_hip, k_knee, k_ankle, target, k_knee + glm::vec3(0, 0, 1));
    EXPECT_TRUE(s.reachable);
    EXPECT_VEC_NEAR(s.end, target, 1e-5F);
    EXPECT_NEAR(glm::length(s.joint - k_hip), glm::length(k_knee - k_hip), 1e-5F);
    EXPECT_NEAR(glm::length(s.end - s.joint), glm::length(k_ankle - k_knee), 1e-5F);
    // The deltas move the bones onto the solved positions.
    EXPECT_VEC_NEAR(k_hip + s.upper * (k_knee - k_hip), s.joint, 1e-5F);
    EXPECT_VEC_NEAR(s.joint + s.lower * (s.upper * (k_ankle - k_knee)), s.end, 1e-5F);
}

TEST(TwoBoneIk, ClampsAnUnreachableTargetToFullExtension) {
    const glm::vec3 target(2.0F, -3.0F, 0.0F);
    const TwoBoneIkSolution s =
        solve_two_bone_ik(k_hip, k_knee, k_ankle, target, k_knee + glm::vec3(0, 0, 1));
    EXPECT_FALSE(s.reachable);
    const float reach = glm::length(k_knee - k_hip) + glm::length(k_ankle - k_knee);
    EXPECT_VEC_NEAR(s.end, k_hip + glm::normalize(target - k_hip) * reach, 1e-5F);
    EXPECT_NEAR(side_of_line(s.joint, target, glm::vec3(0, 0, 1)), 0.0F, 1e-3F)
        << "a fully extended leg is straight";
}

TEST(TwoBoneIk, BendsTheKneeTowardThePole) {
    const glm::vec3 target(0.0F, -0.6F, 0.0F);
    for (const float side : {1.0F, -1.0F}) {
        const glm::vec3 pole(0.0F, -0.3F, side);
        const TwoBoneIkSolution s = solve_two_bone_ik(k_hip, k_knee, k_ankle, target, pole);
        EXPECT_VEC_NEAR(s.end, target, 1e-5F);
        EXPECT_GT(side_of_line(s.joint, target, glm::vec3(0, 0, side)), 0.1F)
            << "knee not on the pole side " << side;
        EXPECT_NEAR(s.joint.x, 0.0F, 1e-5F) << "knee left the hip-target-pole plane";
    }
}

TEST(TwoBoneIk, RewritesTheLocalPoseUnderARotatedRoot) {
    // pelvis -> hip -> knee -> ankle; the whole rig is turned and lifted.
    SkeletonAsset skel;
    skel.joints.resize(4);
    const glm::vec3 offsets[4] = {{0, 1, 0}, {0.1F, 0, 0}, k_knee - k_hip, k_ankle - k_knee};
    for (int i = 0; i < 4; ++i) {
        skel.joints[static_cast<std::size_t>(i)].parent = i - 1;
        skel.joints[static_cast<std::size_t>(i)].translation = offsets[i];
    }
    skel.joints[1].rotation = glm::angleAxis(0.3F, glm::vec3(0, 0, 1));
    const glm::mat4 root = glm::translate(glm::mat4(1.0F), glm::vec3(5, 0, 2)) *
                           glm::rotate(glm::mat4(1.0F), 1.1F, glm::vec3(0, 1, 0));
    std::vector<JointPose> pose = bind_pose(skel);
    const std::vector<glm::mat4> world = pose_world_matrices(skel, pose, root);

    const glm::vec3 ankle(world[3][3]);
    const glm::vec3 target = ankle + glm::vec3(0.1F, 0.25F, -0.05F);
    const glm::vec3 knee(world[2][3]);
    const glm::vec3 pole = knee + (knee - 0.5F * (glm::vec3(world[1][3]) + ankle));
    (void)apply_two_bone_ik(pose, world, {1, 2, 3}, target, pole, 1.0F);

    const std::vector<glm::mat4> solved = pose_world_matrices(skel, pose, root);
    EXPECT_VEC_NEAR(glm::vec3(solved[3][3]), target, 1e-4F);
    EXPECT_VEC_NEAR(glm::vec3(solved[1][3]), glm::vec3(world[1][3]), 1e-5F) << "hip moved";
}

} // namespace
