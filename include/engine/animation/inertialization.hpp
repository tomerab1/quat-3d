#pragma once

// Inertialization (Bollo, "Inertialization: High-Performance Animation
// Transitions in Gears of War", GDC 2018).
//
// A transition records the per-joint offset between the outgoing pose and
// the incoming animation (position, rotation, scale) together with its rate
// of change, then decays that offset to zero with Bollo's quintic over the
// blend time while only the incoming animation is evaluated. The offset
// velocity is source minus target velocity, so the output pose is continuous
// in position and velocity across the switch.

#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/animation/animator.hpp"

namespace engine::animation {

// How a transition moves from one animation to the next.
enum class BlendMode : std::uint8_t {
    inertialize, // evaluate the target only; decay the source offset
    crossfade,   // evaluate both and blend linearly over the blend time
};

// Bollo's quintic for one scalar offset: x(0) = x0, x'(0) = v0, reaching
// zero with zero velocity and acceleration at t1. The duration shrinks so a
// fast approach never overshoots, and the initial acceleration is clamped to
// be non-negative.
struct InertialCurve {
    float x0 = 0.0F;
    float v0 = 0.0F;
    float t1 = 0.0F;
    float a = 0.0F; // t^5 coefficient
    float b = 0.0F; // t^4
    float c = 0.0F; // t^3
    float d = 0.0F; // t^2 (a0 / 2)

    [[nodiscard]] static InertialCurve make(float x0, float v0, float blend_time);
    [[nodiscard]] float evaluate(float t) const;
};

// The decaying offset of one joint: a direction / rotation axis per channel
// plus the scalar curve along it.
struct JointInertialization {
    glm::vec3     translation_dir{0.0F};
    InertialCurve translation;
    glm::vec3     rotation_axis{0.0F, 1.0F, 0.0F};
    InertialCurve rotation; // angle in radians
    glm::vec3     scale_dir{0.0F};
    InertialCurve scale;
};

// Per-character inertialization state (one entry per joint).
struct Inertializer {
    std::vector<JointInertialization> joints;
    float                             elapsed = 0.0F;
    float                             duration = 0.0F;

    [[nodiscard]] bool active() const noexcept {
        return !joints.empty() && elapsed < duration;
    }

    // Start a transition. `source` / `source_prev` are the last two output
    // poses (the previous frame and the one before it, `source_dt` apart);
    // `target` is the incoming animation at the previous frame's instant and
    // `target_next` at this frame's, `target_dt` later. Every span holds one
    // entry per joint. The offset is measured at the previous frame, so call
    // advance(target_dt) before this frame's apply().
    void begin(std::span<const JointPose> source, std::span<const JointPose> source_prev,
               float source_dt, std::span<const JointPose> target,
               std::span<const JointPose> target_next, float target_dt, float blend_time);

    void advance(float dt) noexcept { elapsed += dt; }

    // Add the decayed offset at `elapsed` onto `pose` (the target animation).
    void apply(std::span<JointPose> pose) const;

    void reset() noexcept {
        joints.clear();
        elapsed = 0.0F;
        duration = 0.0F;
    }
};

} // namespace engine::animation
