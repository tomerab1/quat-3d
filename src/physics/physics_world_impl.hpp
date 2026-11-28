#pragma once

// PhysicsWorld internals shared by physics_world.cpp (world, shapes, bodies,
// characters) and physics_queries.cpp (owners, dynamic-body control,
// counters, ray casts): the Jolt state behind the PIMPL, the collision layer
// filters and the glm <-> Jolt conversions. Private to engine_physics.

#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/physics/physics_world.hpp"

// Jolt headers are not clean under our strict warning flags; quiet them here
// (our own code stays under the project's warnings).
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wsuggest-override"
#endif
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace engine::physics {

namespace detail {

// Object layers (must match the Layer enum in the header).
namespace obj_layers {
[[maybe_unused]] constexpr JPH::ObjectLayer k_static = 0;
constexpr JPH::ObjectLayer k_dynamic = 1;
constexpr JPH::ObjectLayer k_character = 2;
[[maybe_unused]] constexpr JPH::ObjectLayer k_trigger = 3; // used from 6.2
constexpr JPH::ObjectLayer k_sensor = 4;
// A character's inner body (G7): gives the CharacterVirtual a presence that
// sensors detect. It overlaps triggers only — the character's own sweep
// already handles every physical contact.
constexpr JPH::ObjectLayer k_character_presence = 5;
} // namespace obj_layers

namespace bp_layers {
constexpr JPH::BroadPhaseLayer k_non_moving{0};
constexpr JPH::BroadPhaseLayer k_moving{1};
constexpr JPH::uint k_count = 2;
} // namespace bp_layers

[[nodiscard]] inline bool is_moving(JPH::ObjectLayer layer) {
    return layer == obj_layers::k_dynamic || layer == obj_layers::k_character ||
           layer == obj_layers::k_character_presence;
}

[[nodiscard]] inline bool is_trigger(JPH::ObjectLayer layer) {
    return layer == obj_layers::k_sensor || layer == obj_layers::k_trigger;
}

class BroadPhaseLayerImpl final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return bp_layers::k_count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return is_moving(layer) ? bp_layers::k_moving : bp_layers::k_non_moving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == bp_layers::k_moving ? "moving" : "non_moving";
    }
#endif
};

class ObjectVsBroadPhaseFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        // Moving broad-phase: everything tests against it. Non-moving broad-phase:
        // only moving objects need to test against it.
        return bp == bp_layers::k_moving || is_moving(layer);
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        // A character presence only meets triggers.
        if (a == obj_layers::k_character_presence) return is_trigger(b);
        if (b == obj_layers::k_character_presence) return is_trigger(a);
        return is_moving(a) || is_moving(b); // at least one body must be moving
    }
};

// Records trigger (sensor) overlaps into a shared queue. Jolt calls these from
// worker threads during Update, so the queue is guarded by a mutex.
class ContactListenerImpl final : public JPH::ContactListener {
public:
    std::vector<PhysicsWorld::TriggerEvent>* events = nullptr;
    std::mutex*                              mutex = nullptr;
    std::unordered_set<std::uint32_t>*       sensors = nullptr;

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold&,
                        JPH::ContactSettings&) override {
        const bool s1 = b1.IsSensor();
        const bool s2 = b2.IsSensor();
        if (!s1 && !s2) return;
        const std::uint32_t id1 = b1.GetID().GetIndexAndSequenceNumber();
        const std::uint32_t id2 = b2.GetID().GetIndexAndSequenceNumber();
        const std::scoped_lock lock(*mutex);
        events->push_back(s1 ? PhysicsWorld::TriggerEvent{id1, id2, true}
                             : PhysicsWorld::TriggerEvent{id2, id1, true});
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        const std::uint32_t id1 = pair.GetBody1ID().GetIndexAndSequenceNumber();
        const std::uint32_t id2 = pair.GetBody2ID().GetIndexAndSequenceNumber();
        const std::scoped_lock lock(*mutex);
        if (sensors->contains(id1)) {
            events->push_back({id1, id2, false});
        } else if (sensors->contains(id2)) {
            events->push_back({id2, id1, false});
        }
    }
};


[[nodiscard]] inline JPH::Vec3 to_jph(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
[[nodiscard]] inline JPH::RVec3 to_jph_r(const glm::vec3& v) {
    return JPH::RVec3(static_cast<JPH::Real>(v.x), static_cast<JPH::Real>(v.y),
                      static_cast<JPH::Real>(v.z));
}
[[nodiscard]] inline JPH::Quat to_jph(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
[[nodiscard]] inline glm::vec3 to_glm(JPH::RVec3Arg v) {
    return {static_cast<float>(v.GetX()), static_cast<float>(v.GetY()),
            static_cast<float>(v.GetZ())};
}
[[nodiscard]] inline glm::quat to_glm(JPH::QuatArg q) {
    return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; // glm::quat is (w, x, y, z)
}

[[nodiscard]] inline JPH::ObjectLayer object_layer_of(Layer layer) {
    return static_cast<JPH::ObjectLayer>(layer);
}

} // namespace detail

struct PhysicsWorld::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl>   temp_allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system;
    detail::BroadPhaseLayerImpl               bp_layer;
    detail::ObjectVsBroadPhaseFilterImpl      obj_vs_bp;
    detail::ObjectLayerPairFilterImpl         obj_pair;
    JPH::PhysicsSystem                        system;
    std::vector<JPH::ShapeRefC>               shapes;
    std::vector<JPH::Ref<JPH::CharacterVirtual>> characters;
    detail::ContactListenerImpl               contact_listener;
    std::vector<PhysicsWorld::TriggerEvent>   trigger_events;
    std::mutex                                trigger_mutex;
    std::unordered_set<std::uint32_t>         sensor_bodies;
    std::unordered_map<std::uint32_t, std::uint32_t> body_owner;       // body -> owner
    std::vector<std::uint32_t>                character_owner;           // parallel to characters
    std::unordered_map<std::uint32_t, std::uint32_t> inner_body_character; // inner body -> character
    float                                     accumulator = 0.0F;

    [[nodiscard]] JPH::CharacterVirtual* character(std::uint32_t id) const {
        return id < characters.size() ? characters[id].GetPtr() : nullptr;
    }
};

} // namespace engine::physics
