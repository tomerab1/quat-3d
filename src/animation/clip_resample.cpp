// Resampling plan for clip compression: the lowest whole multiple of the key
// rate whose interpolated samples reproduce the clip within tolerance.

#include "clip_resample.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::animation {

namespace {

// The clip's own key rate (shortest key interval), snapped to a whole rate.
[[nodiscard]] float key_rate(const AnimClipAsset& clip) {
    float min_dt = std::numeric_limits<float>::infinity();
    for (const AnimSampler& s : clip.samplers) {
        for (std::size_t k = 1; k < s.input.size(); ++k) {
            const float d = s.input[k] - s.input[k - 1];
            if (d > 1e-5F) min_dt = std::min(min_dt, d);
        }
    }
    const float rate = std::isfinite(min_dt) ? 1.0F / min_dt : 30.0F;
    return std::abs(rate - std::round(rate)) < 0.05F ? std::round(rate) : rate;
}

// Per-joint linear interpolation with normalized-lerp rotations (shortest
// arc), as ACL reconstructs between two samples.
[[nodiscard]] std::vector<JointPose> nlerp_poses(const std::vector<JointPose>& a,
                                                 const std::vector<JointPose>& b, float w) {
    std::vector<JointPose> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[i].translation = glm::mix(a[i].translation, b[i].translation, w);
        out[i].scale = glm::mix(a[i].scale, b[i].scale, w);
        const glm::quat q1 = glm::dot(a[i].rotation, b[i].rotation) < 0.0F ? -b[i].rotation
                                                                            : b[i].rotation;
        out[i].rotation = glm::normalize(a[i].rotation * (1.0F - w) + q1 * w);
    }
    return out;
}

// Worst interpolation error at rate `rate` (fractions where nlerp and slerp
// differ most, plus the midpoint).
[[nodiscard]] float interpolation_error(const AnimClipAsset& clip, const SkeletonAsset& skeleton,
                                        float rate, float shell, float give_up) {
    const auto samples =
        static_cast<std::size_t>(std::max(2.0F, std::ceil(clip.duration * rate - 1e-3F) + 1.0F));
    const float step = clip.duration / static_cast<float>(samples - 1);
    float worst = 0.0F;
    std::vector<JointPose> a = sample_local_pose(skeleton, clip, 0.0F);
    for (std::size_t s = 1; s < samples && worst <= give_up; ++s) {
        std::vector<JointPose> b =
            sample_local_pose(skeleton, clip, std::min(static_cast<float>(s) * step, clip.duration));
        for (const float w : {0.21F, 0.5F, 0.79F}) {
            const float t = (static_cast<float>(s - 1) + w) * step;
            worst = std::max(worst, pose_shell_error(skeleton, sample_local_pose(skeleton, clip, t),
                                                     nlerp_poses(a, b, w), shell));
        }
        a = std::move(b);
    }
    return worst;
}

} // namespace

float pose_shell_error(const SkeletonAsset& skeleton, const std::vector<JointPose>& a,
                       const std::vector<JointPose>& b, float shell) {
    const std::vector<glm::mat4> wa = pose_world_matrices(skeleton, a);
    const std::vector<glm::mat4> wb = pose_world_matrices(skeleton, b);
    float worst = 0.0F;
    for (std::size_t j = 0; j < wa.size() && j < wb.size(); ++j) {
        for (int axis = 0; axis < 3; ++axis) {
            for (const float sign : {-1.0F, 1.0F}) {
                glm::vec4 p(0.0F, 0.0F, 0.0F, 1.0F);
                p[axis] = sign * shell;
                worst = std::max(worst, glm::length(glm::vec3(wa[j] * p) - glm::vec3(wb[j] * p)));
            }
        }
    }
    return worst;
}

float plan_sample_rate(const AnimClipAsset& clip, const SkeletonAsset& skeleton, float min_rate,
                       float max_rate, float tolerance, float shell) {
    const float base = key_rate(clip);
    const float lo = std::max(1.0F, std::ceil(min_rate / base - 1e-3F));
    const float hi = std::floor(std::max(max_rate, 1.0F) / base + 1e-3F);
    for (float multiple = lo; multiple <= std::max(hi, lo); multiple += 1.0F) {
        if (interpolation_error(clip, skeleton, base * multiple, shell, tolerance) <= tolerance) {
            return base * multiple;
        }
    }
    return 0.0F;
}

} // namespace engine::animation
