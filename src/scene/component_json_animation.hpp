#pragma once

// Animation components of the component JSON codec (component_json.cpp):
// animator and foot_ik.

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/scene/components.hpp"

namespace engine::scene::detail {

[[nodiscard]] nlohmann::json encode_animator(const Animator& animator);
void decode_animator(core::JsonFields& fields, Animator& animator);

[[nodiscard]] nlohmann::json encode_foot_ik(const FootIK& foot_ik);
void decode_foot_ik(core::JsonFields& fields, FootIK& foot_ik);

} // namespace engine::scene::detail
