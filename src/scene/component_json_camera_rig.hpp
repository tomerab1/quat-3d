#pragma once

// Camera rig components of the component JSON codec (component_json.cpp):
// camera_rig and camera_rig_input (G15.1).

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/scene/camera_rig.hpp"

namespace engine::scene::detail {

[[nodiscard]] nlohmann::json encode_camera_rig(const CameraRig& rig);
void decode_camera_rig(core::JsonFields& fields, CameraRig& rig);

[[nodiscard]] nlohmann::json encode_camera_rig_input(const CameraRigInput& input);
void decode_camera_rig_input(core::JsonFields& fields, CameraRigInput& input);

} // namespace engine::scene::detail
