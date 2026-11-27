#pragma once

// Analytic two-bone IK (hip-knee-ankle, shoulder-elbow-wrist).
//
// The same construction as Unreal's AnimationCore::SolveTwoBoneIK: the
// effector is pulled toward the target (clamped to the chain's reach), the
// middle joint is placed by the law of cosines in the plane of root, target
// and pole, and each bone is rotated onto its new direction by the shortest
// arc. Runs on a local pose between sampling and world composition.

#include <span>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/animation/animator.hpp"

namespace engine::animation {

struct TwoBoneIkSolution {
    glm::vec3 joint{0.0F}; // solved middle joint (knee) position
    glm::vec3 end{0.0F};   // solved effector position; == target when reachable
    // World-space rotation deltas: `upper` rotates the root bone (and its
    // subtree); `lower` then rotates the middle joint's subtree.
    glm::quat upper{1.0F, 0.0F, 0.0F, 0.0F};
    glm::quat lower{1.0F, 0.0F, 0.0F, 0.0F};
    bool      reachable = true;
};

// World-space solve for root `a`, middle `b`, effector `c`. The middle joint
// bends toward `pole` (any point on the side the knee should point to).
[[nodiscard]] TwoBoneIkSolution solve_two_bone_ik(const glm::vec3& a, const glm::vec3& b,
                                                  const glm::vec3& c, const glm::vec3& target,
                                                  const glm::vec3& pole);

// Joint indices of a two-bone chain; `lower` descends from `upper` and `end`
// from `lower` (intermediate twist joints are allowed).
struct TwoBoneChain {
    int upper = -1;
    int lower = -1;
    int end = -1;

    [[nodiscard]] bool valid() const noexcept { return upper >= 0 && lower >= 0 && end >= 0; }
};

// Rotates `chain.upper` / `chain.lower` in the local `pose` so `chain.end`
// reaches `target` (world space), blended by `weight` in [0, 1]. `world`
// holds the pose's world matrices (compute_world_matrices). Returns the
// solution with the deltas actually applied; `end_rotation` receives the
// effector's new world rotation.
TwoBoneIkSolution apply_two_bone_ik(std::span<JointPose> pose, std::span<const glm::mat4> world,
                                    const TwoBoneChain& chain, const glm::vec3& target,
                                    const glm::vec3& pole, float weight,
                                    glm::quat* end_rotation = nullptr);

// Shortest-arc rotation taking unit vector `from` onto unit vector `to`.
[[nodiscard]] glm::quat rotation_between(const glm::vec3& from, const glm::vec3& to);

// Rotation part of a world matrix (columns normalized, so uniform scale is ignored).
[[nodiscard]] glm::quat world_rotation(const glm::mat4& m);

} // namespace engine::animation
