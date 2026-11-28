#pragma once

// The physics components of the component JSON codec (collider, rigidbody,
// character, joint), split out of component_json.cpp by responsibility.
// Private to engine_scene; component_json.cpp dispatches to these.

#include <array>
#include <expected>
#include <string_view>

#include <entt/entity/fwd.hpp>
#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::scene::detail {

// In canonical order (component_infos()).
inline constexpr std::array<std::string_view, 4> k_physics_components = {
    "collider", "rigidbody", "character", "joint"};

[[nodiscard]] bool is_physics_component(std::string_view name);
[[nodiscard]] bool has_physics_component(const entt::registry& registry, entt::entity entity,
                                         std::string_view name);
// Writes out[name] when the entity carries the component.
void encode_physics_component(const entt::registry& registry, entt::entity entity,
                              std::string_view name, nlohmann::json& out);
[[nodiscard]] std::expected<void, core::Error>
apply_physics_component(entt::registry& registry, entt::entity entity, std::string_view name,
                        const nlohmann::json& value, bool strict);
bool remove_physics_component(entt::registry& registry, entt::entity entity,
                              std::string_view name);

} // namespace engine::scene::detail
