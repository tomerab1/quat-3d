// Contact / trigger event collection — see physics_events.hpp.

#include <algorithm>

#include "physics_world_impl.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#endif
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace engine::physics {

using namespace detail;

// ---- Listener (Jolt worker threads) ----------------------------------------

void detail::ContactListenerImpl::OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                                                 const JPH::ContactManifold& manifold,
                                                 JPH::ContactSettings& settings) {
    RawContact c;
    c.body1 = b1.GetID().GetIndexAndSequenceNumber();
    c.body2 = b2.GetID().GetIndexAndSequenceNumber();
    c.trigger = b1.IsSensor() || b2.IsSensor();
    c.sensor1 = b1.IsSensor();
    const JPH::uint points = manifold.mRelativeContactPointsOn1.size();
    JPH::Vec3 sum = JPH::Vec3::sZero();
    for (JPH::uint i = 0; i < points; ++i) sum += manifold.mRelativeContactPointsOn1[i];
    const JPH::RVec3 point =
        manifold.mBaseOffset + (points > 0 ? sum / static_cast<float>(points) : sum);
    c.point = to_glm(point);
    c.normal = to_glm(manifold.mWorldSpaceNormal);
    if (!c.trigger && (b1.IsDynamic() || b2.IsDynamic())) {
        // The solver has not run yet: estimate the impact impulse from the
        // pre-contact velocities (Jolt's helper for impact sounds).
        JPH::CollisionEstimationResult estimate;
        JPH::EstimateCollisionResponse(b1, b2, manifold, estimate, settings.mCombinedFriction,
                                       settings.mCombinedRestitution);
        for (const auto& imp : estimate.mImpulses) c.impulse += imp.mContactImpulse;
    }
    buffers->write().push(c);
}

void detail::ContactListenerImpl::OnContactRemoved(const JPH::SubShapeIDPair& pair) {
    // The bodies cannot be touched here (they may be locked or destroyed).
    RawContact c;
    c.added = false;
    c.body1 = pair.GetBody1ID().GetIndexAndSequenceNumber();
    c.body2 = pair.GetBody2ID().GetIndexAndSequenceNumber();
    c.sensor1 = sensors->contains(c.body1);
    c.trigger = c.sensor1 || sensors->contains(c.body2);
    buffers->write().push(c);
}

// ---- Tracker (main thread) -------------------------------------------------

namespace {

using Kind = PhysicsWorld::ContactEvent::Kind;

} // namespace

void ContactTracker::emit_end(const Pair& p, std::uint64_t step,
                              std::vector<PhysicsWorld::ContactEvent>& out) {
    PhysicsWorld::ContactEvent ev;
    ev.kind = p.trigger ? Kind::trigger_exit : Kind::contact_removed;
    ev.body_a = p.a;
    ev.body_b = p.b;
    ev.owner_a = p.owner_a;
    ev.owner_b = p.owner_b;
    ev.step = step;
    out.push_back(ev);
}

void ContactTracker::process(std::span<const RawContact> raw, std::uint64_t step,
                             const PhysicsWorld& world, const JPH::BodyInterface& bodies,
                             std::vector<PhysicsWorld::ContactEvent>& out) {
    // Adds first: a sub-shape pair handed over between two others in one
    // step must not end the body pair's contact.
    std::vector<std::uint64_t> added; // pairs re-added this step
    for (const RawContact& c : raw) {
        if (!c.added) continue;
        Pair& p = pairs_[key(c.body1, c.body2)];
        if (!p.added_now) added.push_back(key(c.body1, c.body2));
        p.added_now = true;
        if (p.dormant) { // woken up still touching: the contact goes on
            p.dormant = false;
            --dormant_;
            p.touching = 1;
            continue;
        }
        if (p.touching++ > 0) {
            // Another sub-shape of a pair that began this step: keep the
            // strongest impact (O(1): the pair knows its begin event).
            if (p.begin_event != no_event) {
                PhysicsWorld::ContactEvent& ev = out[p.begin_event];
                if (c.impulse > ev.impulse) {
                    ev.impulse = c.impulse;
                    ev.point = c.point;
                    ev.normal = ev.body_a == c.body1 ? c.normal : -c.normal;
                }
            }
            continue;
        }
        // The sensor goes first in trigger events; otherwise Jolt's order.
        const bool swap = c.trigger && !c.sensor1;
        p.a = swap ? c.body2 : c.body1;
        p.b = swap ? c.body1 : c.body2;
        p.owner_a = world.body_owner(p.a);
        p.owner_b = world.body_owner(p.b);
        p.trigger = c.trigger;
        PhysicsWorld::ContactEvent ev;
        ev.kind = c.trigger ? Kind::trigger_enter : Kind::contact_added;
        ev.body_a = p.a;
        ev.body_b = p.b;
        ev.owner_a = p.owner_a;
        ev.owner_b = p.owner_b;
        ev.point = c.point;
        ev.normal = swap ? -c.normal : c.normal;
        ev.impulse = c.impulse;
        ev.step = step;
        p.begin_event = out.size();
        out.push_back(ev);
    }

    for (const RawContact& c : raw) {
        if (c.added) continue;
        const auto it = pairs_.find(key(c.body1, c.body2));
        if (it == pairs_.end()) continue; // began before tracking (or already ended)
        Pair& p = it->second;
        if (p.touching > 0) --p.touching;
        if (p.touching > 0 || p.dormant) continue;
        const JPH::BodyID id1(c.body1);
        const JPH::BodyID id2(c.body2);
        const bool both_exist = bodies.IsAdded(id1) && bodies.IsAdded(id2);
        if (both_exist && (!bodies.IsActive(id1) || !bodies.IsActive(id2)) && !p.added_now) {
            // Jolt drops the contacts of a body that falls asleep; they still
            // touch. Wait: waking up re-adds them, or shows they parted.
            p.dormant = true;
            ++dormant_;
            continue;
        }
        emit_end(p, step, out);
        pairs_.erase(it);
    }

    // Dormant pairs where a body is awake again but no contact was re-added
    // this step have parted.
    if (dormant_ > 0) {
        for (auto it = pairs_.begin(); it != pairs_.end();) {
            const Pair& p = it->second;
            if (p.dormant && !p.added_now &&
                (bodies.IsActive(JPH::BodyID(p.a)) || bodies.IsActive(JPH::BodyID(p.b)))) {
                emit_end(p, step, out);
                --dormant_;
                it = pairs_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const std::uint64_t k : added) {
        if (const auto it = pairs_.find(k); it != pairs_.end()) {
            it->second.added_now = false;
            it->second.begin_event = no_event;
        }
    }
}

void ContactTracker::body_removed(std::uint32_t body, std::uint64_t step,
                                  std::vector<PhysicsWorld::ContactEvent>& out) {
    if (dormant_ == 0) return; // awake contacts end through Jolt's next update
    for (auto it = pairs_.begin(); it != pairs_.end();) {
        const Pair& p = it->second;
        if ((p.a == body || p.b == body) && p.dormant) {
            emit_end(p, step, out);
            --dormant_;
            it = pairs_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace engine::physics
