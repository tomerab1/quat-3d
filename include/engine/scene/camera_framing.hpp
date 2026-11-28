#pragma once

// Camera framing: where to put a camera with a chosen heading and pitch so a
// world-space box fills the view. Exact for the box's eight corners (not a
// bounding sphere), so flat, wide content such as a row of objects fills the
// frame instead of sitting small in its middle. Deterministic: the result
// depends only on the inputs.

#include <glm/glm.hpp>

namespace engine::scene {

struct FramingParams {
    float yaw = 0.0F;   // radians; forward = (cos p cos y, sin p, cos p sin y)
    float pitch = 0.0F; // radians, negative looks down; |pitch| < 89 degrees
    float fov_y = 1.0F; // vertical field of view, radians
    float aspect = 1.0F; // width / height
    float margin = 0.1F; // fraction of the half-extent kept free at each side, [0, 0.9]
    float near_z = 0.1F; // every corner stays at least this far in front
};

// Unit forward vector for (yaw, pitch) in the convention above.
[[nodiscard]] glm::vec3 framing_forward(float yaw, float pitch);

// The closest camera position on the line through the box centre along
// -forward from which every corner of [lo, hi] projects inside the view
// (shrunk by the margin) and lies in front of the near plane. The camera then
// looks along framing_forward(yaw, pitch).
[[nodiscard]] glm::vec3 frame_box(const glm::vec3& lo, const glm::vec3& hi,
                                  const FramingParams& params);

// The yaw / pitch (convention above) a camera with world matrix `world`
// looks along: its forward is -Z of the matrix. Roll is dropped. Looking
// straight up or down, where the forward gives no heading, the yaw comes
// from the camera's right axis instead.
struct FlyAngles {
    float yaw = 0.0F;
    float pitch = 0.0F;
};
[[nodiscard]] FlyAngles fly_angles_from_world(const glm::mat4& world);

} // namespace engine::scene
