// ECS -> physics joints (A10.1): builds a Jolt constraint for every Joint
// component once both bodies exist, and removes constraints whose Joint is
// gone or was edited. See include/engine/scene/scene.hpp (joint_system).

#include <cstdint>

#include <glm/glm.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace engine::scene {

namespace {

using physics::PhysicsWorld;

[[nodiscard]] PhysicsWorld::JointKind kind_of(JointType t) {
    switch (t) {
    case JointType::fixed:
        return PhysicsWorld::JointKind::fixed;
    case JointType::point:
        return PhysicsWorld::JointKind::point;
    case JointType::hinge:
        return PhysicsWorld::JointKind::hinge;
    case JointType::slider:
        return PhysicsWorld::JointKind::slider;
    case JointType::distance:
        return PhysicsWorld::JointKind::distance;
    }
    return PhysicsWorld::JointKind::fixed;
}

[[nodiscard]] PhysicsWorld::MotorMode motor_of(JointMotorMode m) {
    switch (m) {
    case JointMotorMode::off:
        return PhysicsWorld::MotorMode::off;
    case JointMotorMode::velocity:
        return PhysicsWorld::MotorMode::velocity;
    case JointMotorMode::position:
        return PhysicsWorld::MotorMode::position;
    }
    return PhysicsWorld::MotorMode::off;
}

// The live body of `e`'s RigidBody, or invalid_body.
[[nodiscard]] std::uint32_t live_body(const entt::registry& r, const PhysicsWorld& world,
                                      entt::entity e) {
    if (!r.valid(e)) return PhysicsWorld::invalid_body;
    const auto* rb = r.try_get<RigidBody>(e);
    if (rb == nullptr || !world.body_state(rb->body)) return PhysicsWorld::invalid_body;
    return rb->body;
}

[[nodiscard]] glm::vec3 to_world(const entt::registry& r, entt::entity e, const glm::vec3& p) {
    const auto* t = r.try_get<Transform>(e);
    return t != nullptr ? glm::vec3(t->world * glm::vec4(p, 1.0F)) : p;
}

} // namespace

void joint_system(entt::registry& registry, physics::PhysicsWorld& world) {
    // Remove constraints whose owner lost its Joint or now holds another one
    // (an edit resets the handle, like bodies; see physics_system).
    for (const PhysicsWorld::OwnedBody& oj : world.owned_joints()) {
        const auto e = static_cast<entt::entity>(oj.owner);
        const auto* j = registry.valid(e) ? registry.try_get<Joint>(e) : nullptr;
        if (j == nullptr || j->constraint != oj.body) world.remove_joint(oj.body);
    }

    for (auto [e, j] : registry.view<Joint>().each()) {
        if (j.constraint != PhysicsWorld::invalid_body && world.has_joint(j.constraint)) continue;
        // Not built yet, or a body was rebuilt (removing a body removes its
        // joints): (re)build once both bodies exist.
        j.constraint = PhysicsWorld::invalid_body;
        const std::uint32_t a = live_body(registry, world, e);
        if (a == PhysicsWorld::invalid_body) continue;
        std::uint32_t b = PhysicsWorld::invalid_body;
        if (j.target != entt::null) {
            b = live_body(registry, world, j.target);
            if (b == PhysicsWorld::invalid_body) continue; // the target has no body (yet)
        }
        PhysicsWorld::JointParams p;
        p.kind = kind_of(j.type);
        p.body_a = a;
        p.body_b = b;
        p.point_a = to_world(registry, e, j.anchor);
        p.point_b = j.target == entt::null ? j.target_anchor
                                           : to_world(registry, j.target, j.target_anchor);
        const auto* t = registry.try_get<Transform>(e);
        const glm::vec3 axis = t != nullptr ? glm::mat3(t->world) * j.axis : j.axis;
        p.axis = glm::length(axis) > 1e-6F ? glm::normalize(axis) : glm::vec3(0.0F, 1.0F, 0.0F);
        const bool angular = j.type == JointType::hinge;
        p.limited = j.limits_enabled;
        p.limit_min = angular ? glm::radians(j.limits.x) : j.limits.x;
        p.limit_max = angular ? glm::radians(j.limits.y) : j.limits.y;
        p.motor = motor_of(j.motor);
        p.motor_target = angular ? glm::radians(j.motor_target) : j.motor_target;
        p.motor_max_force = j.motor_max_force;
        p.owner = static_cast<std::uint32_t>(entt::to_integral(e));
        j.constraint = world.add_joint(p);
    }
}

} // namespace engine::scene
