#pragma once

// Animator runtime — pose a skeleton from clips (Phase 5, Slices 5.3 + 5.5).
//
// Poses are per-joint local TRS (JointPose). The pipeline is: sample clip(s) into
// poses -> blend/additive-layer them -> compose local matrices -> walk the
// hierarchy -> skinning[j] = joint_world[j] * inverse_bind[j]. A BlendTree bundles
// a 1D blend of two clips plus an optional masked additive layer.

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/core/error.hpp"

namespace engine::animation {

// Local transform of one joint.
struct JointPose {
    glm::vec3 translation{0.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 scale{1.0F};
};

// The skeleton's bind-pose local TRS, one entry per joint.
[[nodiscard]] std::vector<JointPose> bind_pose(const SkeletonAsset& skeleton);

// Sample `clip` at `time` into per-joint local TRS (bind pose for unanimated
// joints; channels override their joint via the node->joint mapping).
[[nodiscard]] std::vector<JointPose>
sample_local_pose(const SkeletonAsset& skeleton, const AnimClipAsset& clip, float time);

// Linear 1D blend: per joint lerp translation/scale and slerp rotation
// (weight 0 -> a, 1 -> b).
[[nodiscard]] std::vector<JointPose>
blend_poses(const std::vector<JointPose>& a, const std::vector<JointPose>& b, float weight);

// Additive layer: `additive` minus `reference` (typically the bind pose), scaled
// by `weight`, applied on top of `base` for joints enabled by `mask` (a bitset,
// joint i = bit i; an empty mask enables every joint).
[[nodiscard]] std::vector<JointPose>
apply_additive(const std::vector<JointPose>& base, const std::vector<JointPose>& additive,
               const std::vector<JointPose>& reference, float weight,
               std::span<const std::uint32_t> mask);

// Compose a pose into skinning matrices (joint_world * inverse_bind).
[[nodiscard]] std::vector<glm::mat4>
pose_skinning_matrices(const SkeletonAsset& skeleton, const std::vector<JointPose>& pose);

// 1D blend of two clips plus an optional masked additive layer, all sampled at
// the same `time`. clip_b/additive may be null.
struct BlendTree {
    const AnimClipAsset* clip_a = nullptr;
    const AnimClipAsset* clip_b = nullptr;
    float                blend = 0.0F; // 0 -> clip_a, 1 -> clip_b

    const AnimClipAsset*       additive = nullptr;
    float                      additive_weight = 0.0F;
    std::vector<std::uint32_t> additive_mask; // empty = all joints

    float time = 0.0F;

    [[nodiscard]] std::vector<glm::mat4> evaluate(const SkeletonAsset& skeleton) const;
};

// --- Single-clip convenience (Slice 5.3) -----------------------------------

[[nodiscard]] std::vector<glm::mat4> bind_skinning_matrices(const SkeletonAsset& skeleton);

[[nodiscard]] std::vector<glm::mat4>
sample_skinning_matrices(const SkeletonAsset& skeleton, const AnimClipAsset& clip, float time);

// Self-tests: single-clip posing (5.3) and blend/additive layering (5.5).
[[nodiscard]] std::expected<void, core::Error> run_animator_self_test();
[[nodiscard]] std::expected<void, core::Error> run_blend_self_test();

} // namespace engine::animation
