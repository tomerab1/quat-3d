#pragma once

// The sdk-consumer game's native code: one component, one system, one remote
// method and one save chunk, written only against the SDK's installed headers.

#include <cstddef>
#include <expected>

#include <entt/entity/fwd.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/error.hpp"

namespace engine::remote {
class RpcServer;
}
namespace engine::scene {
class Scene;
}
namespace engine::game {
class Registrar;
}

namespace consumer {

// Spins its entity about +Y. Plain data; spinner_system holds the logic.
struct Spinner {
    float degrees_per_second = 90.0F;
    float angle_degrees      = 0.0F;
};

// Advances every Spinner by `dt` seconds and writes the rotation into the
// entity's Transform.local (translation kept).
void spinner_system(entt::registry& registry, float dt);

// "consumer.spinners": {} -> {"count": N, "angles": [...]} for the scene's
// spinners, in entity order.
void register_spinner_methods(engine::remote::RpcServer& server, engine::scene::Scene& scene);

// Save chunk: every spinner, keyed by entity name.
[[nodiscard]] nlohmann::json save_spinners(const entt::registry& registry);
// Restores a chunk from save_spinners into same-named entities. The chunk is
// untrusted (a save file): malformed entries fail with a precise error.
[[nodiscard]] std::expected<std::size_t, engine::core::Error>
load_spinners(entt::registry& registry, const nlohmann::json& chunk);

// The native module (G17.1, spinner_module.cpp): Spinner as a field-table
// component, spinner_system in the pre_physics phase, "consumer.count".
[[nodiscard]] std::expected<void, engine::core::Error> register_module(engine::game::Registrar& registrar);

} // namespace consumer
