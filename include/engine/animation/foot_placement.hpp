#pragma once

// Foot placement on uneven ground (two-bone leg IK + pelvis adjustment).
//
// Runs on the sampled local pose, before world composition / skinning. Each
// foot probes the ground straight down from above its animated ankle; the
// ground's height relative to the character's reference plane (the skeleton
// root's origin) becomes that foot's offset, so swing-phase lift is kept.
// The pelvis drops (or rises) by the lower foot's offset, both legs are
// solved with two-bone IK onto their offset targets with the knee bending
// as animated, and planted feet rotate toward the ground normal within a
// limit. Offsets and normals are smoothed by critically damped springs.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "engine/animation/animator.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/two_bone_ik.hpp"

namespace engine::animation {

struct GroundHit {
    glm::vec3 point{0.0F};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
};

// Casts straight down (-Y) from `origin` for `length` metres; nullopt when
// no ground is found (a physics raycast, or a terrain height sample).
using GroundProbe = std::function<std::optional<GroundHit>(const glm::vec3& origin, float length)>;

struct FootPlacementSettings {
    float ray_up = 0.5F;           // probe start above the animated ankle (m)
    float ray_down = 0.8F;         // probe reach below the animated ankle (m)
    float max_adjust = 0.45F;      // largest pelvis / foot offset (m)
    float max_foot_angle = 0.61F;  // largest ground-alignment tilt (radians)
    float smoothing = 0.05F;       // spring half-life (s); 0 snaps
    float plant_height = 0.12F;    // lift above rest at which alignment fades out (m)
};

// Joints driven by foot placement (index 0 = left leg, 1 = right leg).
struct FootRig {
    int                          pelvis = -1;
    std::array<TwoBoneChain, 2>  legs{};
    std::array<float, 2>         ankle_height{}; // rest ankle height above the root plane
    std::array<glm::vec3, 2>     bind_bend{};    // knee bend direction at rest (armature space)
    std::vector<std::uint8_t>    pelvis_subtree; // 1 = joint moves with the pelvis

    [[nodiscard]] bool valid() const noexcept {
        return pelvis >= 0 && legs[0].valid() && legs[1].valid();
    }
};

// Resolves the rig from joint names. Each foot joint names the ankle; its
// parent is the knee and the knee's parent the hip. Empty foot names are
// detected from common conventions (LeftFoot, foot_l, foot.L, ...); an empty
// pelvis name picks the lowest common ancestor of both hips.
// `root_transform` is the skeleton's armature transform (measures the rest
// ankle heights). Returns an invalid rig when a joint cannot be found.
[[nodiscard]] FootRig find_foot_rig(const SkeletonAsset& skeleton, std::string_view pelvis,
                                    std::string_view left_foot, std::string_view right_foot,
                                    const glm::mat4& root_transform = glm::mat4(1.0F));

// Smoothed offsets carried between frames (one per character).
struct FootPlacementState {
    float                    pelvis = 0.0F;
    float                    pelvis_velocity = 0.0F;
    std::array<float, 2>     foot{};
    std::array<float, 2>     foot_velocity{};
    std::array<glm::vec3, 2> normal{glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F)};
    std::array<glm::vec3, 2> knee_bend{}; // last solved bend direction (world), zero = none
    bool                     initialized = false;
};

struct FootPlacementResult {
    float                pelvis_offset = 0.0F;
    std::array<float, 2> foot_offset{};
    std::array<bool, 2>  grounded{};
};

// Adjusts `pose` (local TRS, one per joint) in place. `root_transform` is the
// skeleton's world root, as for pose_skinning_matrices.
FootPlacementResult apply_foot_placement(const SkeletonAsset& skeleton, std::span<JointPose> pose,
                                         const glm::mat4& root_transform, const FootRig& rig,
                                         const FootPlacementSettings& settings,
                                         const GroundProbe& probe, FootPlacementState& state,
                                         float dt);

} // namespace engine::animation
