// PhysicsWorld joints: Jolt two-body constraints (fixed, point, hinge,
// slider, distance) with limits and motors. See
// include/engine/physics/physics_world.hpp; shared internals in
// physics_world_impl.hpp.

#include <cmath>
#include <numbers>
#include <optional>

#include "physics_world_impl.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wsuggest-override"
#endif
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace engine::physics {

using namespace detail;

namespace {

using Kind = PhysicsWorld::JointKind;
using Motor = PhysicsWorld::MotorMode;

// Rejects what Jolt would only assert on (asserts are off in release).
[[nodiscard]] bool valid_params(const PhysicsWorld::JointParams& p) {
    constexpr float pi = std::numbers::pi_v<float>;
    const bool finite = std::isfinite(p.limit_min) && std::isfinite(p.limit_max) &&
                        std::isfinite(p.motor_target) && std::isfinite(p.motor_max_force) &&
                        p.motor_max_force >= 0.0F;
    if (!finite) return false;
    if ((p.kind == Kind::hinge || p.kind == Kind::slider) &&
        !(glm::length(p.axis) > 0.5F && std::isfinite(glm::length(p.axis)))) {
        return false;
    }
    if (p.motor != Motor::off && p.kind != Kind::hinge && p.kind != Kind::slider) return false;
    if (!p.limited) return true;
    switch (p.kind) {
    case Kind::hinge:
        return p.limit_min >= -pi && p.limit_min <= 0.0F && p.limit_max >= 0.0F &&
               p.limit_max <= pi && p.limit_min < p.limit_max;
    case Kind::slider:
        return p.limit_min <= 0.0F && p.limit_max >= 0.0F && p.limit_min < p.limit_max;
    case Kind::distance:
        return p.limit_min >= 0.0F && p.limit_max > 0.0F && p.limit_min <= p.limit_max;
    case Kind::fixed:
    case Kind::point:
        return false; // nothing to limit
    }
    return false;
}

[[nodiscard]] JPH::Ref<JPH::TwoBodyConstraintSettings>
make_settings(const PhysicsWorld::JointParams& p) {
    const JPH::RVec3 a = to_jph_r(p.point_a);
    const JPH::Vec3  axis = to_jph(glm::normalize(p.axis));
    const JPH::Vec3  normal = axis.GetNormalizedPerpendicular();
    switch (p.kind) {
    case Kind::fixed: {
        auto* s = new JPH::FixedConstraintSettings();
        s->mSpace = JPH::EConstraintSpace::WorldSpace;
        s->mAutoDetectPoint = true; // keep the current relative pose
        return s;
    }
    case Kind::point: {
        auto* s = new JPH::PointConstraintSettings();
        s->mSpace = JPH::EConstraintSpace::WorldSpace;
        s->mPoint1 = s->mPoint2 = a;
        return s;
    }
    case Kind::hinge: {
        auto* s = new JPH::HingeConstraintSettings();
        s->mSpace = JPH::EConstraintSpace::WorldSpace;
        s->mPoint1 = s->mPoint2 = a;
        s->mHingeAxis1 = s->mHingeAxis2 = axis;
        s->mNormalAxis1 = s->mNormalAxis2 = normal;
        if (p.limited) {
            s->mLimitsMin = p.limit_min;
            s->mLimitsMax = p.limit_max;
        }
        if (p.motor_max_force > 0.0F) s->mMotorSettings.SetTorqueLimit(p.motor_max_force);
        return s;
    }
    case Kind::slider: {
        auto* s = new JPH::SliderConstraintSettings();
        s->mSpace = JPH::EConstraintSpace::WorldSpace;
        s->mAutoDetectPoint = true;
        s->mSliderAxis1 = s->mSliderAxis2 = axis;
        s->mNormalAxis1 = s->mNormalAxis2 = normal;
        if (p.limited) {
            s->mLimitsMin = p.limit_min;
            s->mLimitsMax = p.limit_max;
        }
        if (p.motor_max_force > 0.0F) s->mMotorSettings.SetForceLimit(p.motor_max_force);
        return s;
    }
    case Kind::distance: {
        auto* s = new JPH::DistanceConstraintSettings();
        s->mSpace = JPH::EConstraintSpace::WorldSpace;
        s->mPoint1 = to_jph_r(p.point_b); // body 1 is the target (or the world)
        s->mPoint2 = a;
        if (p.limited) { // else Jolt keeps the distance the points have now
            s->mMinDistance = p.limit_min;
            s->mMaxDistance = p.limit_max;
        }
        return s;
    }
    }
    return nullptr;
}

void start_motor(JPH::TwoBodyConstraint& c, const PhysicsWorld::JointParams& p) {
    if (p.motor == Motor::off) return;
    const bool velocity = p.motor == Motor::velocity;
    const JPH::EMotorState state =
        velocity ? JPH::EMotorState::Velocity : JPH::EMotorState::Position;
    if (p.kind == Kind::hinge) {
        auto& h = static_cast<JPH::HingeConstraint&>(c); // kind checked: no RTTI needed
        h.SetMotorState(state);
        if (velocity) {
            h.SetTargetAngularVelocity(p.motor_target);
        } else {
            h.SetTargetAngle(p.motor_target);
        }
    } else if (p.kind == Kind::slider) {
        auto& s = static_cast<JPH::SliderConstraint&>(c);
        s.SetMotorState(state);
        if (velocity) {
            s.SetTargetVelocity(p.motor_target);
        } else {
            s.SetTargetPosition(p.motor_target);
        }
    }
}

} // namespace

std::uint32_t PhysicsWorld::add_joint(const JointParams& params) {
    if (!impl_ || !valid_params(params) || params.body_a == params.body_b) return invalid_body;
    const auto state_a = body_state(params.body_a);
    const auto state_b = params.body_b == invalid_body ? std::nullopt : body_state(params.body_b);
    if (!state_a || (params.body_b != invalid_body && !state_b)) return invalid_body;
    if (impl_->inner_bodies.contains(params.body_a) || impl_->inner_bodies.contains(params.body_b)) {
        return invalid_body; // characters are not rigid bodies
    }
    const bool any_dynamic =
        state_a->motion == Motion::dynamic || (state_b && state_b->motion == Motion::dynamic);
    if (!any_dynamic) return invalid_body; // nothing would move

    const JPH::Ref<JPH::TwoBodyConstraintSettings> settings = make_settings(params);
    if (settings == nullptr) return invalid_body;
    const JPH::BodyID id_a(params.body_a);
    const JPH::BodyID id_b = params.body_b == invalid_body ? JPH::BodyID() : JPH::BodyID(params.body_b);
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    // Body 1 is the target (or the world), body 2 this joint's body: Jolt measures
    // hinge angles and slider positions of body 2 relative to body 1.
    JPH::Ref<JPH::TwoBodyConstraint> c = bi.CreateConstraint(settings, id_b, id_a);
    if (c == nullptr) return invalid_body;
    start_motor(*c, params);

    JointRecord rec;
    rec.constraint = c;
    rec.kind = params.kind;
    rec.body_a = params.body_a;
    rec.body_b = params.body_b;
    rec.owner = params.owner;
    rec.local_a = JPH::Vec3(bi.GetCenterOfMassTransform(id_a).Inversed() * to_jph_r(params.point_a));
    rec.local_b = params.body_b == invalid_body
                      ? to_jph(params.point_b)
                      : JPH::Vec3(bi.GetCenterOfMassTransform(id_b).Inversed() *
                                  to_jph_r(params.point_b));
    const std::uint32_t handle = impl_->joints.insert(std::move(rec));
    if (handle == invalid_body) return invalid_body;
    impl_->system.AddConstraint(c);
    bi.ActivateBody(id_a);
    if (!id_b.IsInvalid()) bi.ActivateBody(id_b);
    return handle;
}

void PhysicsWorld::remove_joint(std::uint32_t joint) {
    if (!impl_) return;
    const JointRecord* rec = impl_->joints.get(joint);
    if (rec == nullptr) return;
    impl_->system.RemoveConstraint(rec->constraint);
    // The freed bodies may be resting in a pose the joint held: let them move.
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    for (const std::uint32_t b : {rec->body_a, rec->body_b}) {
        if (b != invalid_body && bi.IsAdded(JPH::BodyID(b))) bi.ActivateBody(JPH::BodyID(b));
    }
    impl_->joints.erase(joint);
}

bool PhysicsWorld::has_joint(std::uint32_t joint) const {
    return impl_ && impl_->joints.contains(joint);
}

std::vector<PhysicsWorld::OwnedBody> PhysicsWorld::owned_joints() const {
    std::vector<OwnedBody> out;
    if (!impl_) return out;
    impl_->joints.for_each([&](std::uint32_t id, const JointRecord& j) {
        if (j.owner != no_owner) out.push_back({id, j.owner});
    });
    return out;
}

std::uint32_t PhysicsWorld::joint_count() const {
    return impl_ ? static_cast<std::uint32_t>(impl_->joints.size()) : 0;
}

std::optional<float> PhysicsWorld::joint_position(std::uint32_t joint) const {
    if (!impl_) return std::nullopt;
    const JointRecord* rec = impl_->joints.get(joint);
    if (rec == nullptr) return std::nullopt;
    switch (rec->kind) {
    case JointKind::hinge:
        return static_cast<const JPH::HingeConstraint&>(*rec->constraint).GetCurrentAngle();
    case JointKind::slider:
        return static_cast<const JPH::SliderConstraint&>(*rec->constraint).GetCurrentPosition();
    case JointKind::distance: {
        const JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
        const JPH::RVec3 a = bi.GetCenterOfMassTransform(JPH::BodyID(rec->body_a)) * rec->local_a;
        const JPH::RVec3 b =
            rec->body_b == invalid_body
                ? JPH::RVec3(rec->local_b)
                : bi.GetCenterOfMassTransform(JPH::BodyID(rec->body_b)) * rec->local_b;
        return static_cast<float>(JPH::Vec3(b - a).Length());
    }
    case JointKind::fixed:
    case JointKind::point:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace engine::physics
