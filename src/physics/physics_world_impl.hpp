#pragma once

// PhysicsWorld internals shared by the engine_physics translation units: the
// Jolt state behind the PIMPL, the collision layer filters, the record tables
// (bodies, shapes, characters) and the glm <-> Jolt conversions. Private to
// engine_physics.

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/physics/physics_world.hpp"
#include "physics_events.hpp"
#include "slot_map.hpp"

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
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
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
[[maybe_unused]] constexpr JPH::ObjectLayer k_trigger = 3;
[[maybe_unused]] constexpr JPH::ObjectLayer k_sensor = 4;
} // namespace obj_layers

namespace bp_layers {
constexpr JPH::BroadPhaseLayer k_non_moving{0};
constexpr JPH::BroadPhaseLayer k_moving{1};
constexpr JPH::uint k_count = 2;
} // namespace bp_layers

[[nodiscard]] inline bool is_moving(JPH::ObjectLayer layer) {
    return layer == obj_layers::k_dynamic || layer == obj_layers::k_character;
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
        return is_moving(a) || is_moving(b); // at least one body must be moving
    }
};

// Records every contact / trigger callback into the current step's buffer
// (physics_events.hpp). Jolt calls these from its worker threads during
// Update; recording is one atomic increment, no lock. `sensors` is only
// modified on the main thread between updates.
class ContactListenerImpl final : public JPH::ContactListener {
public:
    ContactBuffers*                          buffers = nullptr;
    const std::unordered_set<std::uint32_t>* sensors = nullptr;

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& settings) override;
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override;
};

[[nodiscard]] inline JPH::Vec3 to_jph(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
[[nodiscard]] inline JPH::RVec3 to_jph_r(const glm::vec3& v) {
    return JPH::RVec3(static_cast<JPH::Real>(v.x), static_cast<JPH::Real>(v.y),
                      static_cast<JPH::Real>(v.z));
}
[[nodiscard]] inline JPH::Quat to_jph(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
[[nodiscard]] inline glm::vec3 to_glm(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
#ifdef JPH_DOUBLE_PRECISION
[[nodiscard]] inline glm::vec3 to_glm(JPH::RVec3Arg v) {
    return {static_cast<float>(v.GetX()), static_cast<float>(v.GetY()),
            static_cast<float>(v.GetZ())};
}
#endif
[[nodiscard]] inline glm::quat to_glm(JPH::QuatArg q) {
    return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; // glm::quat is (w, x, y, z)
}

[[nodiscard]] inline JPH::ObjectLayer object_layer_of(Layer layer) {
    return static_cast<JPH::ObjectLayer>(layer);
}

// A collision shape and how many live bodies use it. A shape is released
// when its last body goes (or by release_shape when it never had one).
struct ShapeRecord {
    JPH::ShapeRefC shape;
    std::uint32_t  bodies = 0;
};

// What the world remembers about a body it created.
struct BodyRecord {
    std::uint32_t owner = PhysicsWorld::no_owner;
    std::uint32_t shape = PhysicsWorld::invalid_body; // ShapeRecord handle
    bool          sensor = false;
};

// A force / torque that keeps acting for `remaining` simulated seconds.
struct ForceRecord {
    JPH::BodyID body;
    JPH::Vec3   force = JPH::Vec3::sZero();
    JPH::Vec3   torque = JPH::Vec3::sZero();
    JPH::RVec3  point = JPH::RVec3::sZero();
    bool        at_point = false;
    float       remaining = 0.0F;
};

// A joint (Jolt constraint) and the bodies it holds. The local points let
// joint_position() measure a distance joint (Jolt keeps them private).
struct JointRecord {
    JPH::Ref<JPH::TwoBodyConstraint> constraint;
    PhysicsWorld::JointKind          kind = PhysicsWorld::JointKind::fixed;
    std::uint32_t                    body_a = PhysicsWorld::invalid_body;
    std::uint32_t                    body_b = PhysicsWorld::invalid_body; // invalid: the world
    std::uint32_t                    owner = PhysicsWorld::no_owner;
    JPH::Vec3                        local_a = JPH::Vec3::sZero(); // in body a's COM space
    JPH::Vec3                        local_b = JPH::Vec3::sZero(); // body b's, or world space
};

struct CharacterRecord {
    JPH::Ref<JPH::CharacterVirtual> character;
    std::uint32_t                   owner = PhysicsWorld::no_owner;
    // The character's inner rigid body (a kinematic capsule on the CHARACTER
    // layer that follows it): how dynamic bodies and sensors see it.
    std::uint32_t                   inner_body = PhysicsWorld::invalid_body;
};

// What a character's own sweep collides with: everything its layer pair
// filter allows except other characters' inner bodies (the CHARACTER layer):
// characters meet each other through CharacterVsCharacterCollision, which
// sees their exact, current shapes.
class CharacterSweepLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit CharacterSweepLayerFilter(const JPH::ObjectLayerPairFilter& pairs) : pairs_(pairs) {}
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return layer != obj_layers::k_character &&
               pairs_.ShouldCollide(obj_layers::k_character, layer);
    }

private:
    const JPH::ObjectLayerPairFilter& pairs_;
};

} // namespace detail

struct PhysicsWorld::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl>   temp_allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system;
    detail::BroadPhaseLayerImpl               bp_layer;
    detail::ObjectVsBroadPhaseFilterImpl      obj_vs_bp;
    detail::ObjectLayerPairFilterImpl         obj_pair;
    JPH::PhysicsSystem                        system;
    detail::SlotMap<detail::ShapeRecord>      shapes;
    // Every live character is registered here so characters collide with
    // each other (issue #48). Declared before `characters`: it outlives them.
    JPH::CharacterVsCharacterCollisionSimple  character_vs_character;
    detail::SlotMap<detail::CharacterRecord>  characters;
    std::unordered_map<std::uint32_t, std::uint32_t> inner_bodies; // inner body -> character
    std::unordered_map<std::uint32_t, detail::BodyRecord> bodies; // body id -> record
    // Contact events: collected per step on Jolt's workers, tracked per body
    // pair on the main thread (physics_events.hpp).
    detail::ContactBuffers                    contact_buffers;
    detail::ContactListenerImpl               contact_listener;
    detail::ContactTracker                    contact_tracker;
    std::vector<PhysicsWorld::ContactEvent>   contact_events;   // the last update()'s
    std::vector<PhysicsWorld::ContactEvent>   pending_events;   // ended by remove_body
    std::uint32_t                             dropped_contacts = 0;
    std::vector<PhysicsWorld::TriggerEvent>   trigger_events;
    std::unordered_set<std::uint32_t>         sensor_bodies;
    std::uint64_t                             steps = 0;
    float                                     accumulator = 0.0F;
    std::vector<detail::ForceRecord>          forces; // timed forces / torques
    // Declared after `system`: constraints go before the bodies they hold.
    detail::SlotMap<detail::JointRecord>      joints;

    // Removes every joint attached to `body` (before the body is destroyed).
    void remove_joints_of(std::uint32_t body) {
        std::vector<std::uint32_t> doomed;
        joints.for_each([&](std::uint32_t id, const detail::JointRecord& j) {
            if (j.body_a == body || j.body_b == body) doomed.push_back(id);
        });
        for (const std::uint32_t id : doomed) {
            system.RemoveConstraint(joints.get(id)->constraint);
            joints.erase(id);
        }
    }

    // Applies the timed forces for one step of `dt` and retires expired ones
    // (and those of removed or no longer dynamic bodies).
    void apply_forces(float dt) {
        if (forces.empty()) return;
        JPH::BodyInterface& bi = system.GetBodyInterfaceNoLock();
        std::erase_if(forces, [&](detail::ForceRecord& f) {
            if (!bi.IsAdded(f.body) || bi.GetMotionType(f.body) != JPH::EMotionType::Dynamic) {
                return true;
            }
            if (f.at_point) {
                bi.AddForce(f.body, f.force, f.point);
            } else if (!f.force.IsNearZero(0.0F)) {
                bi.AddForce(f.body, f.force);
            }
            if (!f.torque.IsNearZero(0.0F)) bi.AddTorque(f.body, f.torque);
            f.remaining -= dt;
            return f.remaining <= 1e-6F;
        });
    }

    // Fills a query hit's body / character / owner / layer (main thread).
    void describe_hit(const PhysicsWorld& world, JPH::BodyID id, QueryHit& hit) const;

    [[nodiscard]] JPH::CharacterVirtual* character(std::uint32_t id) const {
        const detail::CharacterRecord* c = characters.get(id);
        return c != nullptr ? c->character.GetPtr() : nullptr;
    }

    // Stores a freshly built shape (null = failure) and returns its handle.
    [[nodiscard]] std::uint32_t add_shape(JPH::ShapeRefC shape) {
        if (shape == nullptr) return PhysicsWorld::invalid_body;
        return shapes.insert(detail::ShapeRecord{std::move(shape), 0});
    }

    // Drops one body's reference to `shape`, releasing it with the last one.
    void unref_shape(std::uint32_t shape) {
        detail::ShapeRecord* rec = shapes.get(shape);
        if (rec == nullptr) return;
        if (rec->bodies > 0) --rec->bodies;
        if (rec->bodies == 0) shapes.erase(shape);
    }
};

} // namespace engine::physics
