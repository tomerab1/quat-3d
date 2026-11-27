// Scene-level physics settings — see include/engine/scene/physics_settings.hpp.

#include "engine/scene/physics_settings.hpp"

#include <string>

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"

namespace engine::scene {

PhysicsSettings physics_settings(const entt::registry& registry) {
    const auto* s = registry.ctx().find<PhysicsSettings>();
    return s != nullptr ? *s : PhysicsSettings{};
}

PhysicsSettings& physics_settings_mut(entt::registry& registry) {
    if (auto* s = registry.ctx().find<PhysicsSettings>()) return *s;
    return registry.ctx().emplace<PhysicsSettings>();
}

nlohmann::json encode_physics_settings(const PhysicsSettings& settings) {
    const glm::vec3& g = settings.gravity;
    return {{"gravity", nlohmann::json::array({g.x, g.y, g.z})}};
}

std::expected<void, core::Error> apply_physics_settings_json(PhysicsSettings& settings,
                                                             const nlohmann::json& value,
                                                             bool strict) {
    PhysicsSettings next = settings;
    core::JsonFields f(value, "physics_settings", strict);
    f.vec3("gravity", next.gravity);
    if (glm::length(next.gravity) > k_max_gravity) {
        f.fail("gravity", "magnitude must be at most " +
                              std::to_string(static_cast<int>(k_max_gravity)) +
                              " m/s^2 (Earth is 9.81)");
    }
    if (auto ok = f.finish(); !ok) return ok;
    settings = next;
    return {};
}

} // namespace engine::scene
