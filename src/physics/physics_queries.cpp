// PhysicsWorld queries and gameplay control: body owners, dynamic-body
// impulses / velocities, trigger events, counters and ray casts. See
// include/engine/physics/physics_world.hpp; shared internals in
// physics_world_impl.hpp.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <optional>

#include "physics_world_impl.hpp"

namespace engine::physics {

using namespace detail;

std::uint32_t PhysicsWorld::body_owner(std::uint32_t body) const {
    if (!impl_ || body == invalid_body) return no_owner;
    if (const auto it = impl_->body_owner.find(body); it != impl_->body_owner.end()) {
        return it->second;
    }
    if (const auto it = impl_->inner_body_character.find(body);
        it != impl_->inner_body_character.end() && it->second < impl_->character_owner.size()) {
        return impl_->character_owner[it->second];
    }
    return no_owner;
}

bool PhysicsWorld::is_dynamic(std::uint32_t body) const {
    if (!impl_ || body == invalid_body) return false;
    const JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), JPH::BodyID(body));
    return lock.Succeeded() && lock.GetBody().IsDynamic();
}

void PhysicsWorld::add_impulse(std::uint32_t body, const glm::vec3& impulse) {
    if (!is_dynamic(body)) return;
    impl_->system.GetBodyInterface().AddImpulse(JPH::BodyID(body), to_jph(impulse));
}

void PhysicsWorld::set_linear_velocity(std::uint32_t body, const glm::vec3& velocity) {
    if (!is_dynamic(body)) return;
    impl_->system.GetBodyInterface().SetLinearVelocity(JPH::BodyID(body), to_jph(velocity));
}

glm::vec3 PhysicsWorld::linear_velocity(std::uint32_t body) const {
    if (!impl_ || body == invalid_body) return glm::vec3(0.0F);
    const JPH::Vec3 v = impl_->system.GetBodyInterface().GetLinearVelocity(JPH::BodyID(body));
    return {v.GetX(), v.GetY(), v.GetZ()};
}

std::span<const PhysicsWorld::TriggerEvent> PhysicsWorld::trigger_events() const {
    return {impl_->trigger_events.data(), impl_->trigger_events.size()};
}

// Characters' inner bodies are part of the characters (character_count),
// not bodies of their own; they never sleep, so they are always active.
std::uint32_t PhysicsWorld::body_count() const {
    if (!impl_) return 0;
    return impl_->system.GetNumBodies() -
           static_cast<std::uint32_t>(impl_->inner_body_character.size());
}

std::uint32_t PhysicsWorld::active_body_count() const {
    if (!impl_) return 0;
    const std::uint32_t active = impl_->system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    const auto inner = static_cast<std::uint32_t>(impl_->inner_body_character.size());
    return active > inner ? active - inner : 0;
}

std::uint32_t PhysicsWorld::character_count() const {
    if (!impl_) return 0;
    std::uint32_t n = 0;
    for (const auto& c : impl_->characters) n += c != nullptr ? 1U : 0U;
    return n;
}

namespace {
// Skips sensor (trigger) bodies unless asked to include them, bodies whose
// owner is ignored, and characters' inner bodies (cast_ray tests character
// shapes itself).
class RayBodyFilter final : public JPH::BodyFilter {
public:
    RayBodyFilter(bool include_sensors, std::span<const std::uint32_t> ignore_owners,
                  const std::unordered_map<std::uint32_t, std::uint32_t>& body_owner,
                  const std::unordered_map<std::uint32_t, std::uint32_t>& inner_bodies)
        : include_sensors_(include_sensors), ignore_owners_(ignore_owners),
          body_owner_(body_owner), inner_bodies_(inner_bodies) {}
    [[nodiscard]] bool ShouldCollide(const JPH::BodyID& id) const override {
        return !inner_bodies_.contains(id.GetIndexAndSequenceNumber());
    }
    [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override {
        if (!include_sensors_ && body.IsSensor()) return false;
        if (ignore_owners_.empty()) return true;
        const auto it = body_owner_.find(body.GetID().GetIndexAndSequenceNumber());
        return it == body_owner_.end() || !is_ignored(ignore_owners_, it->second);
    }

    [[nodiscard]] static bool is_ignored(std::span<const std::uint32_t> ignore,
                                         std::uint32_t owner) {
        return std::find(ignore.begin(), ignore.end(), owner) != ignore.end();
    }

private:
    bool                                                    include_sensors_;
    std::span<const std::uint32_t>                          ignore_owners_;
    const std::unordered_map<std::uint32_t, std::uint32_t>& body_owner_;
    const std::unordered_map<std::uint32_t, std::uint32_t>& inner_bodies_;
};
} // namespace

std::optional<PhysicsWorld::RayHit> PhysicsWorld::cast_ray(const glm::vec3& origin,
                                                           const glm::vec3& direction,
                                                           float max_distance,
                                                           bool include_sensors,
                                                           std::span<const std::uint32_t> ignore_owners)
    const {
    const float len = glm::length(direction);
    if (!impl_ || !(len > 1e-6F) || !(max_distance > 0.0F) || !std::isfinite(max_distance)) {
        return std::nullopt;
    }
    const glm::vec3 dir = direction / len;
    const JPH::RRayCast ray(to_jph_r(origin), to_jph(dir * max_distance));
    std::optional<RayHit> best;
    float best_fraction = 1.0F + FLT_EPSILON;

    JPH::RayCastResult result;
    const RayBodyFilter filter(include_sensors, ignore_owners, impl_->body_owner,
                               impl_->inner_body_character);
    if (impl_->system.GetNarrowPhaseQuery().CastRay(ray, result, JPH::BroadPhaseLayerFilter{},
                                                    JPH::ObjectLayerFilter{}, filter)) {
        RayHit hit;
        hit.body = result.mBodyID.GetIndexAndSequenceNumber();
        const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
        hit.point = to_glm(point);
        hit.distance = result.mFraction * max_distance;
        const JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            const JPH::Vec3 n = body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point);
            hit.normal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
            hit.sensor = body.IsSensor();
        }
        if (const auto it = impl_->body_owner.find(hit.body); it != impl_->body_owner.end()) {
            hit.owner = it->second;
        }
        best_fraction = result.mFraction;
        best = hit;
    }

    // Character controllers are not bodies: test their shapes directly, in
    // each character's centre-of-mass space.
    for (std::size_t i = 0; i < impl_->characters.size(); ++i) {
        const JPH::CharacterVirtual* c = impl_->characters[i].GetPtr();
        if (c == nullptr ||
            RayBodyFilter::is_ignored(ignore_owners, impl_->character_owner[i])) {
            continue;
        }
        const JPH::RMat44 com = c->GetCenterOfMassTransform();
        const JPH::RRayCast local_r = ray.Transformed(com.InversedRotationTranslation());
        const JPH::RayCast local(JPH::Vec3(local_r.mOrigin), local_r.mDirection);
        JPH::RayCastResult ch;
        ch.mFraction = best_fraction;
        if (!c->GetShape()->CastRay(local, JPH::SubShapeIDCreator(), ch) ||
            ch.mFraction >= best_fraction) {
            continue;
        }
        RayHit hit;
        hit.character = static_cast<std::uint32_t>(i);
        hit.owner = impl_->character_owner[i];
        hit.point = to_glm(ray.GetPointOnRay(ch.mFraction));
        hit.distance = ch.mFraction * max_distance;
        const JPH::Vec3 n = com.Multiply3x3(
            c->GetShape()->GetSurfaceNormal(ch.mSubShapeID2, local.GetPointOnRay(ch.mFraction)));
        hit.normal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
        best_fraction = ch.mFraction;
        best = hit;
    }
    return best;
}

} // namespace engine::physics
