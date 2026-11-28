#pragma once

// Coin Collector's native module (ROADMAP G17.1): the proof that a game
// registers its own component, system, remote method and Lua module through
// engine::game::Registrar, built against the SDK's public headers only.
//
//   coin_spin            component (field table): spin and bob a coin
//   coin_collector.spin  pre_physics system: animates every coin_spin entity
//                        (its kinematic body follows the same step)
//   coins.status         remote method: how many coins are left
//   require("coin_collector").ring_position(i, count, radius)
//                        Lua: where coin i of a ring goes
// The rest of the game (score, pickups, the player) stays in Lua.

#include <cstdint>
#include <expected>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/game/module.hpp"

namespace coin_collector {

struct CoinSpin {
    float     spin_speed = 180.0F; // degrees per second about the vertical axis
    float     bob_height = 0.15F;  // metres
    float     bob_rate = 2.0F;     // radians of bob phase per second
    // Runtime state, captured on the first update (read-only in JSON).
    bool      anchored = false;
    glm::vec3 base{0.0F};          // rest position
    glm::vec3 scale{1.0F};
    float     phase = 0.0F;
    float     angle = 0.0F;
};

// The pre_physics system (exposed for the tests).
void spin_coins(engine::game::SystemContext& context, float dt);

// Coin `index` of `count` on a ring of `radius` metres, 1 m above the ground.
[[nodiscard]] glm::vec3 ring_position(std::int32_t index, std::int32_t count, float radius);

[[nodiscard]] std::expected<void, engine::core::Error> register_module(engine::game::Registrar& registrar);

} // namespace coin_collector
