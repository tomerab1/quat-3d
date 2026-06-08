#pragma once

// Animator runtime — pose a skeleton from a clip (Phase 5, Slice 5.3).
//
// Pure functions that turn (skeleton, clip, time) into the per-joint skinning
// matrices the GPU skinning pass (5.4) consumes: skinning[j] = joint_world[j] *
// inverse_bind[j], where joint_world is produced by sampling the clip's channels
// into per-joint local TRS (falling back to the bind pose for unanimated joints)
// and walking the skeleton hierarchy. The ECS glue (Animator component +
// AnimationSystem advancing time) lives in the scene module.

#include <expected>
#include <vector>

#include <glm/glm.hpp>

#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/core/error.hpp"

namespace engine::animation {

// Skinning matrices for the skeleton's bind pose (no animation). Equals identity
// per joint when inverse_bind is the inverse of the bind-pose world transform.
[[nodiscard]] std::vector<glm::mat4> bind_skinning_matrices(const SkeletonAsset& skeleton);

// Skinning matrices for `clip` sampled at `time` (seconds). Channels override the
// bind-pose local TRS of their target joints; everything else holds bind pose.
[[nodiscard]] std::vector<glm::mat4>
sample_skinning_matrices(const SkeletonAsset& skeleton, const AnimClipAsset& clip, float time);

// Builds a two-joint skeleton + a translation clip in code and verifies the
// skinning matrices at the bind time (identity) and at a posed time.
[[nodiscard]] std::expected<void, core::Error> run_animator_self_test();

} // namespace engine::animation
