#pragma once

// Exact critically damped springs in half-life form (Holden, "Spring-It-On:
// The Game Developer's Spring-Roll-Call", 2021; Lowe, "Critically Damped
// Ease-In/Ease-Out Smoothing", Game Programming Gems 4, 2004).
//
// The closed-form solution is frame-rate independent (one step of dt equals
// two of dt/2 up to rounding) and never overshoots a fixed goal. `half_life`
// is Holden's: the time in which the spring's exponential envelope halves
// (damping = 4 ln 2 / half_life). Used by foot placement (G8) and the camera
// rig (G15.1).

#include <cmath>
#include <numbers>

#include <glm/glm.hpp>

namespace engine::core {

// Moves `x` (with velocity `v`) toward `goal` over `dt` seconds. A half-life
// of zero or less snaps to the goal.
inline void spring_damper(float& x, float& v, float goal, float half_life, float dt) {
    if (!(half_life > 0.0F)) {
        x = goal;
        v = 0.0F;
        return;
    }
    // y = damping / 2, damping = 4 ln 2 / half_life.
    const float y = 2.0F * std::numbers::ln2_v<float> / half_life;
    const float j0 = x - goal;
    const float j1 = v + j0 * y;
    const float e = std::exp(-y * dt);
    x = e * (j0 + j1 * dt) + goal;
    v = e * (v - j1 * y * dt);
}

template <glm::length_t N>
inline void spring_damper(glm::vec<N, float>& x, glm::vec<N, float>& v,
                          const glm::vec<N, float>& goal, float half_life, float dt) {
    for (glm::length_t i = 0; i < N; ++i) spring_damper(x[i], v[i], goal[i], half_life, dt);
}

// The angle in (-180, 180] degrees equivalent to `deg`.
[[nodiscard]] inline float wrap_degrees(float deg) {
    float w = std::fmod(deg + 180.0F, 360.0F);
    if (w < 0.0F) w += 360.0F;
    return w - 180.0F;
}

// A spring on an angle in degrees, along the shortest arc to `goal`.
inline void spring_damper_degrees(float& x, float& v, float goal, float half_life, float dt) {
    spring_damper(x, v, x + wrap_degrees(goal - x), half_life, dt);
}

} // namespace engine::core
