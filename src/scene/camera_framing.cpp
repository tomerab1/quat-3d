// Camera framing — see engine/scene/camera_framing.hpp.

#include "engine/scene/camera_framing.hpp"

#include <algorithm>
#include <cmath>

namespace engine::scene {

glm::vec3 framing_forward(float yaw, float pitch) {
    return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
}

glm::vec3 frame_box(const glm::vec3& lo, const glm::vec3& hi, const FramingParams& params) {
    const float limit = glm::radians(89.0F);
    const glm::vec3 fwd = framing_forward(params.yaw, glm::clamp(params.pitch, -limit, limit));
    const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0F, 1.0F, 0.0F)));
    const glm::vec3 up = glm::cross(right, fwd);

    const float keep = 1.0F - glm::clamp(params.margin, 0.0F, 0.9F);
    const float fov_y = glm::clamp(params.fov_y, glm::radians(1.0F), glm::radians(179.0F));
    const float ty = std::tan(0.5F * fov_y) * keep;
    const float tx = std::tan(0.5F * fov_y) * std::max(params.aspect, 1e-3F) * keep;
    const float near_z = std::max(params.near_z, 0.0F);

    // A corner q (relative to the centre) at depth d + dot(q, fwd) is inside
    // the view when |dot(q, right)| <= tx * depth and |dot(q, up)| <= ty *
    // depth: each corner bounds d from below.
    const glm::vec3 centre = 0.5F * (lo + hi);
    float distance = 0.0F;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y,
                               (i & 4) ? hi.z : lo.z);
        const glm::vec3 q = corner - centre;
        const float along = glm::dot(q, fwd);
        distance = std::max({distance, std::abs(glm::dot(q, right)) / tx - along,
                             std::abs(glm::dot(q, up)) / ty - along, near_z - along});
    }
    return centre - fwd * distance;
}

FlyAngles fly_angles_from_world(const glm::mat4& world) {
    const glm::vec3 fwd = -glm::normalize(glm::vec3(world[2]));
    FlyAngles out;
    out.pitch = std::asin(glm::clamp(fwd.y, -1.0F, 1.0F));
    if (glm::length(glm::vec2(fwd.x, fwd.z)) > 1e-3F) {
        out.yaw = std::atan2(fwd.z, fwd.x);
    } else {
        // right = (-sin yaw, 0, cos yaw) for an unrolled camera.
        const glm::vec3 right(world[0]);
        out.yaw = std::atan2(-right.x, right.z);
    }
    return out;
}

} // namespace engine::scene
