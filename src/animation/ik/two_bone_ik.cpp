// Analytic two-bone IK (law of cosines + shortest-arc bone rotations).

#include "engine/animation/two_bone_ik.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>

namespace engine::animation {

namespace {

constexpr float k_epsilon = 1e-6F;

// Any unit vector perpendicular to unit `v`.
[[nodiscard]] glm::vec3 any_perpendicular(const glm::vec3& v) {
    const glm::vec3 axis =
        std::abs(v.x) < 0.9F ? glm::vec3(1.0F, 0.0F, 0.0F) : glm::vec3(0.0F, 1.0F, 0.0F);
    return glm::normalize(glm::cross(v, axis));
}

// Component of `v` perpendicular to unit `dir`, normalized; zero if parallel.
[[nodiscard]] glm::vec3 perpendicular_part(const glm::vec3& v, const glm::vec3& dir) {
    const glm::vec3 p = v - glm::dot(v, dir) * dir;
    const float len = glm::length(p);
    return len > k_epsilon ? p / len : glm::vec3(0.0F);
}

[[nodiscard]] glm::vec3 safe_normalize(const glm::vec3& v, const glm::vec3& fallback) {
    const float len = glm::length(v);
    return len > k_epsilon ? v / len : fallback;
}

} // namespace

glm::quat rotation_between(const glm::vec3& from, const glm::vec3& to) {
    const float d = glm::dot(from, to);
    if (d < -1.0F + 1e-6F) {
        return glm::angleAxis(glm::pi<float>(), any_perpendicular(from)); // opposite
    }
    const glm::vec3 c = glm::cross(from, to);
    return glm::normalize(glm::quat(1.0F + d, c.x, c.y, c.z));
}

glm::quat world_rotation(const glm::mat4& m) {
    const glm::mat3 r(safe_normalize(glm::vec3(m[0]), glm::vec3(1.0F, 0.0F, 0.0F)),
                      safe_normalize(glm::vec3(m[1]), glm::vec3(0.0F, 1.0F, 0.0F)),
                      safe_normalize(glm::vec3(m[2]), glm::vec3(0.0F, 0.0F, 1.0F)));
    return glm::normalize(glm::quat_cast(r));
}

TwoBoneIkSolution solve_two_bone_ik(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                    const glm::vec3& target, const glm::vec3& pole,
                                    const glm::vec3& bend_hint) {
    TwoBoneIkSolution s;
    const float upper_len = glm::length(b - a);
    const float lower_len = glm::length(c - b);
    if (upper_len < k_epsilon || lower_len < k_epsilon) {
        s.joint = b;
        s.end = c;
        s.reachable = false;
        return s;
    }

    const glm::vec3 to_target = target - a;
    const float dist = glm::length(to_target);
    const float max_len = upper_len + lower_len;
    const float min_len = std::abs(upper_len - lower_len);
    s.reachable = dist <= max_len * (1.0F + 1e-5F) && dist >= min_len * (1.0F - 1e-5F);
    const glm::vec3 dir = safe_normalize(to_target, safe_normalize(c - a, {0.0F, -1.0F, 0.0F}));
    const float reach = std::clamp(dist, std::max(min_len, k_epsilon), max_len);

    // Bend plane: root, target and pole; then the current knee, the hint,
    // and only as a last resort an arbitrary perpendicular.
    glm::vec3 bend = perpendicular_part(pole - a, dir);
    if (bend == glm::vec3(0.0F)) bend = perpendicular_part(b - a, dir);
    if (bend == glm::vec3(0.0F)) bend = perpendicular_part(bend_hint, dir);
    if (bend == glm::vec3(0.0F)) bend = any_perpendicular(dir);

    const float cos_upper = std::clamp(
        (upper_len * upper_len + reach * reach - lower_len * lower_len) /
            (2.0F * upper_len * reach),
        -1.0F, 1.0F);
    const float sin_upper = std::sqrt(std::max(0.0F, 1.0F - cos_upper * cos_upper));
    s.joint = a + upper_len * (cos_upper * dir + sin_upper * bend);
    s.end = a + reach * dir;

    s.upper = rotation_between((b - a) / upper_len, glm::normalize(s.joint - a));
    const glm::vec3 lower_dir = s.upper * ((c - b) / lower_len);
    s.lower = rotation_between(glm::normalize(lower_dir),
                               safe_normalize(s.end - s.joint, lower_dir));
    return s;
}

TwoBoneIkSolution apply_two_bone_ik(std::span<JointPose> pose, std::span<const glm::mat4> world,
                                    const TwoBoneChain& chain, const glm::vec3& target,
                                    const glm::vec3& pole, float weight,
                                    glm::quat* end_rotation, const glm::vec3& bend_hint) {
    const auto in_range = [&](int i) {
        return i >= 0 && static_cast<std::size_t>(i) < pose.size() &&
               static_cast<std::size_t>(i) < world.size();
    };
    if (!chain.valid() || !in_range(chain.upper) || !in_range(chain.lower) ||
        !in_range(chain.end)) {
        return {};
    }
    const auto up = static_cast<std::size_t>(chain.upper);
    const auto lo = static_cast<std::size_t>(chain.lower);
    const auto en = static_cast<std::size_t>(chain.end);
    const glm::vec3 a(world[up][3]);
    const glm::vec3 b(world[lo][3]);
    const glm::vec3 c(world[en][3]);
    const float w = std::clamp(weight, 0.0F, 1.0F);
    const TwoBoneIkSolution s =
        solve_two_bone_ik(a, b, c, glm::mix(c, target, w), pole, bend_hint);

    // local' = local * W^-1 * D * W for a world delta D on a joint whose
    // parent did not move; the lower joint's parent moved by `upper`.
    const glm::quat w_up = world_rotation(world[up]);
    const glm::quat w_lo = world_rotation(world[lo]);
    pose[up].rotation =
        glm::normalize(pose[up].rotation * glm::inverse(w_up) * s.upper * w_up);
    pose[lo].rotation = glm::normalize(pose[lo].rotation * glm::inverse(w_lo) *
                                       glm::inverse(s.upper) * s.lower * s.upper * w_lo);
    if (end_rotation != nullptr) {
        *end_rotation = glm::normalize(s.lower * s.upper * world_rotation(world[en]));
    }
    return s;
}

} // namespace engine::animation
