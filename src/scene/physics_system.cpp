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
