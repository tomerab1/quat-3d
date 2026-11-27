#include "engine/net/character_step.hpp"

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::net {

CharacterState character_state(const entt::registry& registry, entt::entity character) {
    const auto& cc = registry.get<scene::CharacterController>(character);
    const auto& t = registry.get<scene::Transform>(character);
    return {glm::vec3(t.world[3]), cc.velocity, cc.on_ground};
}

void restore_character(entt::registry& registry, physics::PhysicsWorld& world,
                       entt::entity character, const CharacterState& state) {
    auto& cc = registry.get<scene::CharacterController>(character);
    auto& t = registry.get<scene::Transform>(character);
    // character_system writes translation-only transforms for characters.
    t.local[3] = glm::vec4(state.position, 1.0F);
    t.world[3] = glm::vec4(state.position, 1.0F);
    // A character not created yet spawns at the Transform on its first step.
    if (cc.character != physics::PhysicsWorld::invalid_body &&
        world.character_position(cc.character) != state.position) {
        world.character_set_position(cc.character, state.position);
    }
    cc.velocity = state.velocity;
    cc.on_ground = state.on_ground;
}

CharacterState step_character_input(entt::registry& registry, physics::PhysicsWorld& world,
                                    entt::entity character, const CharacterState& state,
                                    const PlayerInput& input) {
    restore_character(registry, world, character, state);
    registry.get<scene::CharacterController>(character).move = input.move;
    scene::step_character(registry, world, character, input.dt);
    return character_state(registry, character);
}

} // namespace engine::net
