// PhysicsWorld body control: state reads, impulses, timed forces, velocities,
// motion type and surface / motion tuning (A10.1; G7 scripts use the same
// calls). See include/engine/physics/physics_world.hpp; shared internals in
// physics_world_impl.hpp.

#include <algorithm>
#include <cmath>
#include <optional>

#include "physics_world_impl.hpp"

namespace engine::physics {

using namespace detail;

namespace {

[[nodiscard]] Motion motion_of(JPH::EMotionType m) {
    switch (m) {
    case JPH::EMotionType::Static:
        return Motion::static_body;
    case JPH::EMotionType::Kinematic:
        return Motion::kinematic;
    case JPH::EMotionType::Dynamic:
        return Motion::dynamic;
    }
    return Motion::static_body;
}

} // namespace

int PhysicsWorld::force_steps(float duration_s) {
    constexpr int max_steps = static_cast<int>(max_force_duration / fixed_timestep + 0.5F);
    if (!std::isfinite(duration_s)) return 1;
    const float steps = std::round(duration_s / fixed_timestep);
    return std::clamp(static_cast<int>(std::min(steps, static_cast<float>(max_steps))), 1,
                      max_steps);
}

std::optional<PhysicsWorld::BodyState> PhysicsWorld::body_state(std::uint32_t body) const {
    if (!impl_ || body == invalid_body) return std::nullopt;
    const JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), JPH::BodyID(body));
    if (!lock.Succeeded() || !lock.GetBody().IsInBroadPhase()) return std::nullopt;
    const JPH::Body& b = lock.GetBody();
    BodyState s;
    s.motion = motion_of(b.GetMotionType());
    s.layer = static_cast<Layer>(b.GetObjectLayer());
    s.sensor = b.IsSensor();
    s.awake = b.IsActive();
    s.position = to_glm(b.GetCenterOfMassPosition());
    s.rotation = to_glm(b.GetRotation());
    s.friction = b.GetFriction();
    s.restitution = b.GetRestitution();
    if (const JPH::MotionProperties* mp = b.GetMotionPropertiesUnchecked(); mp != nullptr) {
        s.linear_velocity = to_glm(mp->GetLinearVelocity());
        s.angular_velocity = to_glm(mp->GetAngularVelocity());
        s.gravity_factor = mp->GetGravityFactor();
        s.linear_damping = mp->GetLinearDamping();
        s.angular_damping = mp->GetAngularDamping();
        if (b.IsDynamic() && mp->GetInverseMass() > 0.0F) s.mass = 1.0F / mp->GetInverseMass();
    }
    if (b.IsStatic()) { // a static body never moves, whatever it was built with
        s.linear_velocity = glm::vec3(0.0F);
        s.angular_velocity = glm::vec3(0.0F);
    }
    return s;
}

bool PhysicsWorld::is_dynamic(std::uint32_t body) const {
    if (!impl_ || body == invalid_body) return false;
    const JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), JPH::BodyID(body));
    return lock.Succeeded() && lock.GetBody().IsInBroadPhase() && lock.GetBody().IsDynamic();
}

void PhysicsWorld::add_impulse(std::uint32_t body, const glm::vec3& impulse) {
    if (!is_dynamic(body)) return;
    impl_->system.GetBodyInterface().AddImpulse(JPH::BodyID(body), to_jph(impulse));
}

void PhysicsWorld::add_impulse(std::uint32_t body, const glm::vec3& impulse,
                               const glm::vec3& world_point) {
    if (!is_dynamic(body)) return;
    impl_->system.GetBodyInterface().AddImpulse(JPH::BodyID(body), to_jph(impulse),
                                                to_jph_r(world_point));
}

void PhysicsWorld::add_angular_impulse(std::uint32_t body, const glm::vec3& impulse) {
    if (!is_dynamic(body)) return;
    impl_->system.GetBodyInterface().AddAngularImpulse(JPH::BodyID(body), to_jph(impulse));
}

void PhysicsWorld::add_force(std::uint32_t body, const glm::vec3& force, float duration_s) {
    if (!is_dynamic(body)) return;
    impl_->forces.push_back({.body = JPH::BodyID(body),
                             .force = to_jph(force),
                             .steps_left = force_steps(duration_s)});
    impl_->system.GetBodyInterface().ActivateBody(JPH::BodyID(body));
}

void PhysicsWorld::add_force(std::uint32_t body, const glm::vec3& force,
                             const glm::vec3& world_point, float duration_s) {
    if (!is_dynamic(body)) return;
    impl_->forces.push_back({.body = JPH::BodyID(body),
                             .force = to_jph(force),
                             .point = to_jph_r(world_point),
                             .at_point = true,
                             .steps_left = force_steps(duration_s)});
    impl_->system.GetBodyInterface().ActivateBody(JPH::BodyID(body));
}

void PhysicsWorld::add_torque(std::uint32_t body, const glm::vec3& torque, float duration_s) {
    if (!is_dynamic(body)) return;
    impl_->forces.push_back({.body = JPH::BodyID(body),
                             .torque = to_jph(torque),
                             .steps_left = force_steps(duration_s)});
    impl_->system.GetBodyInterface().ActivateBody(JPH::BodyID(body));
}

void PhysicsWorld::set_linear_velocity(std::uint32_t body, const glm::vec3& velocity) {
    if (!is_dynamic(body)) return;
    impl_->system.GetBodyInterface().SetLinearVelocity(JPH::BodyID(body), to_jph(velocity));
}

glm::vec3 PhysicsWorld::linear_velocity(std::uint32_t body) const {
    const auto s = body_state(body);
    return s ? s->linear_velocity : glm::vec3(0.0F);
}

void PhysicsWorld::set_angular_velocity(std::uint32_t body, const glm::vec3& radians_per_s) {
    if (!is_dynamic(body)) return;
    impl_->system.GetBodyInterface().SetAngularVelocity(JPH::BodyID(body),
                                                        to_jph(radians_per_s));
}

glm::vec3 PhysicsWorld::angular_velocity(std::uint32_t body) const {
    const auto s = body_state(body);
    return s ? s->angular_velocity : glm::vec3(0.0F);
}

bool PhysicsWorld::set_motion(std::uint32_t body, Motion motion) {
    if (!impl_ || body == invalid_body || motion == Motion::static_body) return false;
    const JPH::BodyID id(body);
    {
        const JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), id);
        if (!lock.Succeeded() || !lock.GetBody().CanBeKinematicOrDynamic()) return false;
    }
    impl_->system.GetBodyInterface().SetMotionType(
        id, motion == Motion::dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Kinematic,
        JPH::EActivation::Activate);
    return true;
}

void PhysicsWorld::set_friction(std::uint32_t body, float friction) {
    if (!body_state(body)) return;
    impl_->system.GetBodyInterface().SetFriction(JPH::BodyID(body), friction);
}

void PhysicsWorld::set_restitution(std::uint32_t body, float restitution) {
    if (!body_state(body)) return;
    impl_->system.GetBodyInterface().SetRestitution(JPH::BodyID(body), restitution);
}

void PhysicsWorld::set_gravity_factor(std::uint32_t body, float factor) {
    if (!body_state(body)) return;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    bi.SetGravityFactor(JPH::BodyID(body), factor);
    bi.ActivateBody(JPH::BodyID(body)); // a resting body must feel the change
}

void PhysicsWorld::set_damping(std::uint32_t body, float linear, float angular) {
    if (!impl_ || body == invalid_body) return;
    const JPH::BodyLockWrite lock(impl_->system.GetBodyLockInterface(), JPH::BodyID(body));
    if (!lock.Succeeded()) return;
    if (JPH::MotionProperties* mp = lock.GetBody().GetMotionPropertiesUnchecked()) {
        mp->SetLinearDamping(linear);
        mp->SetAngularDamping(angular);
    }
}

void PhysicsWorld::set_awake(std::uint32_t body, bool awake) {
    if (!body_state(body)) return;
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    if (awake) {
        bi.ActivateBody(JPH::BodyID(body));
    } else {
        bi.DeactivateBody(JPH::BodyID(body));
    }
}

} // namespace engine::physics
