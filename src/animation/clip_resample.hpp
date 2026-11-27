#pragma once

// Uniform resampling plan for clip compression (private to engine_animation).

#include <vector>

#include "engine/animation/animator.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"

namespace engine::animation {

// The rate (Hz) to resample `clip` at before compression: a whole multiple
// of its key rate (every key lands on a sample) in [min_rate, max_rate],
// the lowest one at which normalized-lerp interpolation between samples
// (what ACL decompression does) stays within `tolerance` of the clip's own
// sampling, measured as object-space distance on a `shell` around each joint
// (skeleton units). 0 when no rate up to `max_rate` is good enough.
[[nodiscard]] float plan_sample_rate(const AnimClipAsset& clip, const SkeletonAsset& skeleton,
                                     float min_rate, float max_rate, float tolerance, float shell);

// Largest distance between matching virtual vertices (6 per joint, `shell`
// along the joint's axes) of two poses, in the skeleton's object space.
[[nodiscard]] float pose_shell_error(const SkeletonAsset& skeleton,
                                     const std::vector<JointPose>& a,
                                     const std::vector<JointPose>& b, float shell);

} // namespace engine::animation
