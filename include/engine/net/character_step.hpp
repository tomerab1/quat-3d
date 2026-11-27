#pragma once

// The networked character step (S1 audit B1 / B5). Host and client run the
// exact same rule — scene::step_character on the entity's CharacterController
// and its Jolt CharacterVirtual — once per player input. The client's
// prediction, rewind and replay all go through the CharacterVirtual (moved
// with PhysicsWorld::character_set_position), so the controller, its
// collision state and the Transform never disagree.

#include <entt/entity/fwd.hpp>

#include "engine/net/prediction.hpp"

namespace engine::physics {
class PhysicsWorld;
}

namespace engine::net {

// Current state of `character` (CharacterController + Transform required).
[[nodiscard]] CharacterState character_state(const entt::registry& registry,
                                             entt::entity character);

// Put `character` (controller, CharacterVirtual, Transform) into `state`.
// Moving the CharacterVirtual is skipped when it is already there.
void restore_character(entt::registry& registry, physics::PhysicsWorld& world,
                       entt::entity character, const CharacterState& state);

// restore_character(state), apply `input`, run one character step of
// input.dt, and return the resulting state. Usable as a
// CharacterPredictor::StepFn (bind registry / world / character).
[[nodiscard]] CharacterState step_character_input(entt::registry& registry,
                                                  physics::PhysicsWorld& world,
                                                  entt::entity character,
                                                  const CharacterState& state,
                                                  const PlayerInput& input);

} // namespace engine::net
