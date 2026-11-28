#pragma once

// dynamic_mesh component of the component JSON codec (component_json.cpp;
// ROADMAP R23.2).

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/scene/dynamic_mesh.hpp"

namespace engine::scene::detail {

[[nodiscard]] nlohmann::json encode_dynamic_mesh(const DynamicMesh& mesh);
void decode_dynamic_mesh(core::JsonFields& fields, DynamicMesh& mesh);

} // namespace engine::scene::detail
