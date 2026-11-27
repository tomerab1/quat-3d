// Foot placement: feet meet steps and slopes, the pelvis drops to the lower
// foot, offsets clamp and smooth, and rigs resolve from common joint names.

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/animation/foot_placement.hpp"
#include "support/glm_near.hpp"

namespace {

using namespace engine::animation;

// Hips at y = 1, legs 0.1 m either side, knees bent forward, ankles 0.1 m
// above the root plane (y = 0) at rest.
[[nodiscard]] SkeletonAsset legs_skeleton(const std::string& prefix = "") {
    SkeletonAsset skel;
    const auto add = [&](const std::string& name, int parent, glm::vec3 t) {
        Joint j;
        j.name = prefix + name;
        j.parent = parent;
        j.translation = t;
        skel.joints.push_back(j);
    };
    add("Hips", -1, {0.0F, 1.0F, 0.0F});
    add("LeftUpLeg", 0, {0.1F, 0.0F, 0.0F});
    add("LeftLeg", 1, {0.0F, -0.45F, 0.08F});
    add("LeftFoot", 2, {0.0F, -0.45F, -0.08F});
    add("RightUpLeg", 0, {-0.1F, 0.0F, 0.0F});
    add("RightLeg", 4, {0.0F, -0.45F, 0.08F});
    add("RightFoot", 5, {0.0F, -0.45F, -0.08F});
    add("Spine", 0, {0.0F, 0.2F, 0.0F});
    return skel;
}

constexpr std::size_t k_hips = 0;
constexpr std::size_t k_left_foot = 3;
constexpr std::size_t k_right_foot = 6;

// A heightfield probe: ground at height(x, z) with the matching normal.
[[nodiscard]] GroundProbe heightfield(std::function<float(float, float)> height) {
    return [height](const glm::vec3& origin, float length) -> std::optional<GroundHit> {
        const float h = height(origin.x, origin.z);
        if (h > origin.y || h < origin.y - length) return std::nullopt;
        const float e = 1e-3F;
        const float dx = (height(origin.x + e, origin.z) - height(origin.x - e, origin.z)) / (2 * e);
        const float dz = (height(origin.x, origin.z + e) - height(origin.x, origin.z - e)) / (2 * e);
        return GroundHit{{origin.x, h, origin.z}, glm::normalize(glm::vec3(-dx, 1.0F, -dz))};
    };
}

struct Placed {
    std::vector<glm::mat4> world;
    FootPlacementResult    result;
};

[[nodiscard]] Placed place(const SkeletonAsset& skel, const GroundProbe& probe,
                           FootPlacementState& state, FootPlacementSettings settings = {},
                           float dt = 1.0F / 60.0F) {
    const FootRig rig = find_foot_rig(skel, "", "", "");
    EXPECT_TRUE(rig.valid());
    std::vector<JointPose> pose = bind_pose(skel);
    Placed out;
    out.result = apply_foot_placement(skel, pose, glm::mat4(1.0F), rig, settings, probe, state, dt);
    out.world = pose_world_matrices(skel, pose);
    return out;
}

[[nodiscard]] glm::vec3 position(const Placed& p, std::size_t joint) {
    return glm::vec3(p.world[joint][3]);
}

TEST(FootPlacement, FeetMeetAStepAndThePelvisDropsToTheLowerFoot) {
    const SkeletonAsset skel = legs_skeleton();
    // Left foot on y = 0, right foot on a step down to y = -0.2.
    const GroundProbe step = heightfield([](float x, float) { return x > 0.0F ? 0.0F : -0.2F; });
    FootPlacementState state;
    const Placed p = place(skel, step, state);

    EXPECT_NEAR(p.result.pelvis_offset, -0.2F, 1e-5F);
    EXPECT_NEAR(position(p, k_hips).y, 0.8F, 1e-5F);
    EXPECT_NEAR(position(p, k_left_foot).y, 0.1F, 1e-4F) << "left ankle off its ground";
    EXPECT_NEAR(position(p, k_right_foot).y, -0.1F, 1e-4F) << "right ankle off its ground";
    // Feet only move vertically.
    EXPECT_NEAR(position(p, k_left_foot).x, 0.1F, 1e-4F);
    EXPECT_NEAR(position(p, k_right_foot).z, 0.0F, 1e-4F);
}

TEST(FootPlacement, FeetMeetASlopeAndTiltToItsNormal) {
    const SkeletonAsset skel = legs_skeleton();
    const GroundProbe slope = heightfield([](float x, float) { return 0.3F * x; });
    FootPlacementState state;
    const Placed p = place(skel, slope, state);

    EXPECT_NEAR(p.result.pelvis_offset, -0.03F, 1e-5F);
    EXPECT_NEAR(position(p, k_left_foot).y, 0.3F * 0.1F + 0.1F, 1e-4F);
    EXPECT_NEAR(position(p, k_right_foot).y, -0.3F * 0.1F + 0.1F, 1e-4F);
    const glm::vec3 normal = glm::normalize(glm::vec3(-0.3F, 1.0F, 0.0F));
    for (const std::size_t foot : {k_left_foot, k_right_foot}) {
        const glm::vec3 foot_up = glm::normalize(glm::vec3(p.world[foot][1]));
        EXPECT_VEC_NEAR(foot_up, normal, 1e-3F) << "foot " << foot << " not aligned";
    }
}

TEST(FootPlacement, TiltIsLimitedToTheMaximumAngle) {
    const SkeletonAsset skel = legs_skeleton();
    const GroundProbe steep = heightfield([](float x, float) { return 1.0F * x; }); // 45 deg
    FootPlacementSettings settings;
    settings.max_foot_angle = glm::radians(20.0F);
    FootPlacementState state;
    const Placed p = place(skel, steep, state, settings);
    const glm::vec3 foot_up = glm::normalize(glm::vec3(p.world[k_left_foot][1]));
    EXPECT_NEAR(glm::degrees(std::acos(foot_up.y)), 20.0F, 0.05F);
}

TEST(FootPlacement, OffsetsClampToMaxAdjust) {
    const SkeletonAsset skel = legs_skeleton();
    const GroundProbe cliff = heightfield([](float x, float) { return x > 0.0F ? 0.0F : -0.7F; });
    FootPlacementSettings settings;
    settings.ray_down = 2.0F;
    FootPlacementState state;
    const Placed p = place(skel, cliff, state, settings);
    EXPECT_NEAR(p.result.pelvis_offset, -settings.max_adjust, 1e-5F);
    EXPECT_NEAR(position(p, k_right_foot).y, 0.1F - settings.max_adjust, 1e-4F);
}

TEST(FootPlacement, NoGroundLeavesTheAnimatedPose) {
    const SkeletonAsset skel = legs_skeleton();
    FootPlacementState state;
    const Placed p = place(skel, GroundProbe{}, state);
    EXPECT_FALSE(p.result.grounded[0]);
    EXPECT_NEAR(position(p, k_hips).y, 1.0F, 1e-6F);
    EXPECT_NEAR(position(p, k_left_foot).y, 0.1F, 1e-5F);
}

TEST(FootPlacement, SpringsSmoothAChangeInGround) {
    const SkeletonAsset skel = legs_skeleton();
    FootPlacementState state;
    FootPlacementSettings settings;
    settings.smoothing = 0.05F;
    (void)place(skel, heightfield([](float, float) { return 0.0F; }), state, settings);

    const GroundProbe step = heightfield([](float x, float) { return x > 0.0F ? 0.0F : -0.2F; });
    const Placed first = place(skel, step, state, settings);
    EXPECT_GT(first.result.pelvis_offset, -0.2F + 0.05F) << "no smoothing";
    EXPECT_LT(first.result.pelvis_offset, 0.0F);
    Placed last = first;
    for (int i = 0; i < 60; ++i) last = place(skel, step, state, settings);
    EXPECT_NEAR(last.result.pelvis_offset, -0.2F, 1e-3F);
    EXPECT_NEAR(position(last, k_right_foot).y, -0.1F, 2e-3F);
}

TEST(FootRig, ResolvesCommonNamingConventions) {
    for (const std::string prefix : {"", "mixamorig:"}) {
        const SkeletonAsset skel = legs_skeleton(prefix);
        const FootRig rig = find_foot_rig(skel, "", "", "");
        ASSERT_TRUE(rig.valid()) << prefix;
        EXPECT_EQ(rig.pelvis, 0);
        EXPECT_EQ(rig.legs[0].upper, 1);
        EXPECT_EQ(rig.legs[0].lower, 2);
        EXPECT_EQ(rig.legs[0].end, 3);
        EXPECT_EQ(rig.legs[1].end, 6);
        EXPECT_NEAR(rig.ankle_height[0], 0.1F, 1e-6F);
    }
    SkeletonAsset ue = legs_skeleton();
    ue.joints[3].name = "foot_l";
    ue.joints[6].name = "foot_r";
    EXPECT_TRUE(find_foot_rig(ue, "", "", "").valid());
    EXPECT_FALSE(find_foot_rig(ue, "", "missing", "foot_r").valid());
    EXPECT_FALSE(find_foot_rig(ue, "Spine", "", "").valid()) << "pelvis must be above the hips";
}

TEST(FootRig, RestBendIsInArmatureSpaceWhateverTheRoot) {
    const SkeletonAsset skel = legs_skeleton();
    const FootRig plain = find_foot_rig(skel, "", "", "");
    ASSERT_TRUE(plain.valid());
    EXPECT_VEC_NEAR(plain.bind_bend[0], glm::vec3(0.0F, 0.0F, 1.0F), 1e-5F) << "knees bend forward";

    // Placed, turned and scaled like a Mixamo armature (centimetres -> metres).
    const glm::mat4 root = glm::translate(glm::mat4(1.0F), glm::vec3(12.0F, -3.0F, 40.0F)) *
                           glm::rotate(glm::mat4(1.0F), 2.1F, glm::vec3(0.0F, 1.0F, 0.0F)) *
                           glm::rotate(glm::mat4(1.0F), 0.4F, glm::vec3(1.0F, 0.0F, 0.0F)) *
                           glm::scale(glm::mat4(1.0F), glm::vec3(0.01F));
    const FootRig placed = find_foot_rig(skel, "", "", "", root);
    ASSERT_TRUE(placed.valid());
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_VEC_NEAR(placed.bind_bend[i], plain.bind_bend[i], 1e-5F) << "leg " << i;
    }
}

} // namespace
