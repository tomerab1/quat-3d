// ECS <-> physics bridge system (Phase 6, Slice 6.2).

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::scene {

namespace {

[[nodiscard]] std::uint32_t make_shape(physics::PhysicsWorld& world, const Collider& col) {
    switch (col.shape) {
    case ColliderShape::box:
        return world.create_box(col.half_extents);
    case ColliderShape::sphere:
        return world.create_sphere(col.half_extents.x);
    case ColliderShape::capsule:
        return world.create_capsule(col.half_extents.y, col.half_extents.x);
    }
    return world.create_box(col.half_extents);
}

[[nodiscard]] physics::Motion to_motion(BodyMotion m) {
    switch (m) {
    case BodyMotion::static_body:
        return physics::Motion::static_body;
    case BodyMotion::kinematic:
        return physics::Motion::kinematic;
    case BodyMotion::dynamic:
        return physics::Motion::dynamic;
    }
    return physics::Motion::dynamic;
}

[[nodiscard]] glm::vec3 position_of(const Transform& t) { return glm::vec3(t.local[3]); }
[[nodiscard]] glm::quat rotation_of(const Transform& t) { return glm::quat_cast(glm::mat3(t.local)); }

} // namespace

void physics_system(entt::registry& registry, physics::PhysicsWorld& world, float dt) {
    const std::uint32_t invalid = physics::PhysicsWorld::invalid_body;

    // 1. Create a body for each entity that has a Collider but no body yet.
    for (auto [e, rb, col, t] : registry.view<RigidBody, Collider, Transform>().each()) {
        if (rb.body != invalid) continue;
        physics::PhysicsWorld::BodyParams params;
        params.shape = make_shape(world, col);
        params.position = position_of(t);
        params.rotation = rotation_of(t);
        params.motion = to_motion(rb.motion);
        params.layer = rb.motion == BodyMotion::static_body ? physics::Layer::static_body
                                                            : physics::Layer::dynamic;
        params.mass = rb.mass;
        rb.body = world.add_body(params);
    }

    // 2. Push kinematic bodies' transforms into the simulation.
    for (auto [e, rb, t] : registry.view<RigidBody, Transform>().each()) {
        if (rb.motion == BodyMotion::kinematic && rb.body != invalid) {
            world.set_body_transform(rb.body, position_of(t), rotation_of(t));
        }
    }

    // 3. Step the simulation.
    world.update(dt);

    // 4. Pull dynamic bodies' transforms back into the ECS (entities are roots).
    for (auto [e, rb, t] : registry.view<RigidBody, Transform>().each()) {
        if (rb.motion == BodyMotion::dynamic && rb.body != invalid) {
            glm::vec3 pos(0.0F);
            glm::quat rot(1.0F, 0.0F, 0.0F, 0.0F);
            world.body_transform(rb.body, pos, rot);
            t.local = glm::translate(glm::mat4(1.0F), pos) * glm::mat4_cast(rot);
            t.world = t.local;
        }
    }
}

void character_system(entt::registry& registry, physics::PhysicsWorld& world, float dt) {
    const std::uint32_t invalid = physics::PhysicsWorld::invalid_body;
    constexpr float gravity = -9.81F;

    for (auto [e, cc, t] : registry.view<CharacterController, Transform>().each()) {
        if (cc.character == invalid) {
            cc.character = world.create_character(cc.half_height, cc.radius, position_of(t));
        }

        glm::vec3 velocity = cc.velocity;
        if (cc.on_ground && velocity.y < 0.0F) {
            velocity.y = 0.0F; // grounded: stop accumulating downward speed
        }
        velocity.y += gravity * dt;
        velocity.x = cc.move.x * cc.speed;
        velocity.z = cc.move.z * cc.speed;

        world.character_set_velocity(cc.character, velocity);
        world.character_update(cc.character, dt);

        cc.velocity = world.character_velocity(cc.character);
        cc.on_ground = world.character_on_ground(cc.character);
        const glm::vec3 pos = world.character_position(cc.character);
        t.local = glm::translate(glm::mat4(1.0F), pos);
        t.world = t.local;
    }
}

std::expected<void, core::Error> run_character_self_test() {
    auto world = physics::PhysicsWorld::create();
    if (!world) return std::unexpected(world.error());

    // Static floor (top at y=0), added directly to the world.
    physics::PhysicsWorld::BodyParams floor;
    floor.shape = world->create_box(glm::vec3(50.0F, 1.0F, 50.0F));
    floor.position = glm::vec3(0.0F, -1.0F, 0.0F);
    floor.motion = physics::Motion::static_body;
    floor.layer = physics::Layer::static_body;
    (void)world->add_body(floor);

    entt::registry registry;
    const entt::entity e = registry.create();
    registry.emplace<Transform>(e).local =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 5.0F, 0.0F));
    registry.emplace<CharacterController>(e);

    // Fall onto the floor (4 s).
    for (int i = 0; i < 240; ++i) {
        character_system(registry, *world, 1.0F / 60.0F);
    }
    auto& cc = registry.get<CharacterController>(e);
    const float landed_y = registry.get<Transform>(e).local[3].y;
    if (landed_y > 4.0F) {
        return std::unexpected(core::Error{"character self-test: did not fall"});
    }
    if (!cc.on_ground) {
        return std::unexpected(core::Error{"character self-test: not grounded after landing"});
    }
    if (landed_y < 0.0F) {
        return std::unexpected(core::Error{"character self-test: sank through floor"});
    }

    // Walk +X for 1 s and confirm horizontal motion.
    const float start_x = registry.get<Transform>(e).local[3].x;
    cc.move = glm::vec3(1.0F, 0.0F, 0.0F);
    for (int i = 0; i < 60; ++i) {
        character_system(registry, *world, 1.0F / 60.0F);
    }
    const float end_x = registry.get<Transform>(e).local[3].x;
    if (end_x - start_x < 1.0F) {
        return std::unexpected(core::Error{"character self-test: did not walk forward"});
    }
    return {};
}

std::expected<void, core::Error> run_physics_body_self_test() {
    auto world = physics::PhysicsWorld::create();
    if (!world) return std::unexpected(world.error());

    entt::registry registry;

    // Static floor: 100x2x100 box centred at y=-1 (top at y=0).
    const entt::entity floor = registry.create();
    registry.emplace<Transform>(floor).local =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, -1.0F, 0.0F));
    registry.emplace<Collider>(floor, ColliderShape::box, glm::vec3(50.0F, 1.0F, 50.0F),
                               glm::vec3(0.0F));
    registry.emplace<RigidBody>(floor, BodyMotion::static_body, 1.0F,
                                physics::PhysicsWorld::invalid_body);

    // Dynamic sphere (radius 0.5) dropped from y=5.
    const entt::entity sphere = registry.create();
    registry.emplace<Transform>(sphere).local =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 5.0F, 0.0F));
    registry.emplace<Collider>(sphere, ColliderShape::sphere, glm::vec3(0.5F), glm::vec3(0.0F));
    registry.emplace<RigidBody>(sphere, BodyMotion::dynamic, 1.0F,
                                physics::PhysicsWorld::invalid_body);

    for (int i = 0; i < 120; ++i) {
        physics_system(registry, *world, 1.0F / 60.0F);
    }

    const float y = registry.get<Transform>(sphere).local[3].y;
    if (y > 4.5F) {
        return std::unexpected(core::Error{"physics body self-test: sphere did not fall"});
    }
    if (y < 0.0F) {
        return std::unexpected(core::Error{"physics body self-test: sphere fell through floor"});
    }
    if (std::abs(y - 0.5F) > 0.25F) {
        return std::unexpected(core::Error{"physics body self-test: sphere not resting on floor"});
    }
    return {};
}

} // namespace engine::scene
