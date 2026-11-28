#pragma once

// Scene-level physics settings (ROADMAP A10.1): one gravity vector used by
// Jolt and by the character controllers. It is scene data, not per-entity:
// it lives in the registry's context (entt ctx), travels with the scene
// (scene files' optional, versioned "physics_settings" block, the editor's
// play snapshot) and is pushed into the PhysicsWorld by physics_system /
// character_system every tick.

#include <expected>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::scene {

struct PhysicsSettings {
    glm::vec3 gravity{0.0F, -9.81F, 0.0F}; // m/s^2, world space
};

// Largest accepted gravity magnitude, m/s^2.
inline constexpr float k_max_gravity = 1000.0F;
// Version written into scene files' "physics_settings" block.
inline constexpr int k_physics_settings_version = 1;

// The scene's settings (the defaults when none were set).
[[nodiscard]] PhysicsSettings physics_settings(const entt::registry& registry);
// The scene's settings for writing (created with the defaults on first use).
[[nodiscard]] PhysicsSettings& physics_settings_mut(entt::registry& registry);

// {"gravity": [x, y, z]} (no version; scene_io adds it).
[[nodiscard]] nlohmann::json encode_physics_settings(const PhysicsSettings& settings);
// Merges `value` into `settings` through core::JsonFields: `strict` rejects
// unknown fields (remote), files decode leniently. `settings` is untouched
// on error.
[[nodiscard]] std::expected<void, core::Error>
apply_physics_settings_json(PhysicsSettings& settings, const nlohmann::json& value, bool strict);

} // namespace engine::scene
