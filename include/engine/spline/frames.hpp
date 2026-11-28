#pragma once

// Frames along a curve (ROADMAP G18.1): an orthonormal basis at each sample,
// for sweeping profiles, placing posts and orienting instances.
//
//   up_locked          tangent projected onto XZ, up = world +Y. Walls and
//                      fences stay vertical whatever the slope.
//   rotation_minimising the double-reflection method (W. Wang, B. Jüttler,
//                      D. Zheng, Y. Liu, "Computation of Rotation Minimizing
//                      Frames", ACM Transactions on Graphics 27(1), 2008):
//                      the frame twists as little as possible, for rails on
//                      slopes, ivy and ribbons. No flips on helices.
//
// right = cross(tangent, up): looking along the curve with +Y up, right
// points to the right-hand side. Widths and offsets are measured along it.
//
// Limitation: on a closed, non-planar curve the rotation-minimising frame
// does not return to its start after one lap (holonomy), so a ribbon shows
// a twist step at the seam. Distributing that angle over the lap (Wang et
// al. 2008, section 6) needs trigonometry and is not done yet.

#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "engine/spline/curve.hpp"

namespace engine::spline {

enum class FrameMode : std::uint8_t { up_locked, rotation_minimising };

struct Frame {
    glm::vec3     position{0.0F};
    glm::vec3     tangent{1.0F, 0.0F, 0.0F};
    glm::vec3     up{0.0F, 1.0F, 0.0F};
    glm::vec3     right{0.0F, 0.0F, 1.0F};
    float         width = 1.0F;
    float         height = 0.0F;
    std::uint32_t tags = 0;
    float         param = 0.0F;
    float         distance = 0.0F;

    // Column-major rotation + translation: X = tangent, Y = up, Z = right.
    [[nodiscard]] glm::mat4 matrix() const;
};

// Frames at the given samples, in order. Rotation-minimising frames start
// from the up-locked frame at the first sample (world up projected
// perpendicular to the tangent) and propagate sample to sample, so pass
// samples dense enough for the curve's turning (sample_by_spacing).
// `world_up` must be non-zero; it defaults to +Y.
[[nodiscard]] std::vector<Frame> frames(std::span<const CurveSample> samples, FrameMode mode,
                                        const glm::vec3& world_up = glm::vec3(0.0F, 1.0F, 0.0F));

// sample_by_spacing + frames in one call.
[[nodiscard]] std::vector<Frame> sample_frames(const Curve& curve, const SpacingOptions& spacing, FrameMode mode,
                                               const glm::vec3& world_up = glm::vec3(0.0F, 1.0F, 0.0F));

// One up-locked frame (no propagation needed).
[[nodiscard]] Frame up_locked_frame(const CurveSample& sample,
                                    const glm::vec3& world_up = glm::vec3(0.0F, 1.0F, 0.0F));

[[nodiscard]] std::string_view frame_mode_name(FrameMode mode);
[[nodiscard]] bool             parse_frame_mode(std::string_view name, FrameMode& out);

} // namespace engine::spline
