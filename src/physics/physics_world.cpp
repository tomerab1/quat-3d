#include "engine/physics/physics_world.hpp"

#include <cfloat>
#include <cmath>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Jolt headers are not clean under our strict warning flags; quiet them here
// (our own code in this TU stays under the project's warnings).
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

namespace {

// Object layers (must match the Layer enum in the header).
namespace obj_layers {
[[maybe_unused]] constexpr JPH::ObjectLayer k_static = 0;
constexpr JPH::ObjectLayer k_dynamic = 1;
constexpr JPH::ObjectLayer k_character = 2;
[[maybe_unused]] constexpr JPH::ObjectLayer k_trigger = 3; // used from 6.2
[[maybe_unused]] constexpr JPH::ObjectLayer k_sensor = 4;  // used from 6.2
} // namespace obj_layers

namespace bp_layers {
constexpr JPH::BroadPhaseLayer k_non_moving{0};
constexpr JPH::BroadPhaseLayer k_moving{1};
constexpr JPH::uint k_count = 2;
} // namespace bp_layers

[[nodiscard]] bool is_moving(JPH::ObjectLayer layer) {
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

// Jolt's global state (allocator, factory, type registration) initialised once
// for the process. Never torn down — it lives for the program's lifetime.
void ensure_jolt_initialized() {
    static const bool done = [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
    (void)done;
}

[[nodiscard]] JPH::Vec3 to_jph(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
[[nodiscard]] JPH::RVec3 to_jph_r(const glm::vec3& v) {
    return JPH::RVec3(static_cast<JPH::Real>(v.x), static_cast<JPH::Real>(v.y),
                      static_cast<JPH::Real>(v.z));
}
[[nodiscard]] JPH::Quat to_jph(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
[[nodiscard]] glm::vec3 to_glm(JPH::RVec3Arg v) {
    return {static_cast<float>(v.GetX()), static_cast<float>(v.GetY()),
            static_cast<float>(v.GetZ())};
}
[[nodiscard]] glm::quat to_glm(JPH::QuatArg q) {
    return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; // glm::quat is (w, x, y, z)
}

[[nodiscard]] JPH::ObjectLayer object_layer_of(Layer layer) {
    return static_cast<JPH::ObjectLayer>(layer);
}

} // namespace

struct PhysicsWorld::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl>   temp_allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system;
    BroadPhaseLayerImpl                       bp_layer;
    ObjectVsBroadPhaseFilterImpl              obj_vs_bp;
    ObjectLayerPairFilterImpl                 obj_pair;
    JPH::PhysicsSystem                        system;
    std::vector<JPH::ShapeRefC>               shapes;
    std::vector<JPH::Ref<JPH::CharacterVirtual>> characters;
    ContactListenerImpl                       contact_listener;
    std::vector<PhysicsWorld::TriggerEvent>   trigger_events;
    std::mutex                                trigger_mutex;
    std::unordered_set<std::uint32_t>         sensor_bodies;
    std::unordered_map<std::uint32_t, std::uint32_t> body_owner;       // body -> owner
    std::vector<std::uint32_t>                character_owner;           // parallel to characters
    float                                     accumulator = 0.0F;

    [[nodiscard]] JPH::CharacterVirtual* character(std::uint32_t id) const {
        return id < characters.size() ? characters[id].GetPtr() : nullptr;
    }
};

std::expected<PhysicsWorld, core::Error> PhysicsWorld::create() {
    ensure_jolt_initialized();

    PhysicsWorld world;
    world.impl_ = std::make_unique<Impl>();
    Impl& m = *world.impl_;

    m.temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
    const unsigned hw = std::thread::hardware_concurrency();
    const int threads = static_cast<int>(hw > 1 ? hw - 1 : 1);
    m.job_system = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs,
                                                              JPH::cMaxPhysicsBarriers, threads);

    constexpr JPH::uint max_bodies = 65536;
    constexpr JPH::uint num_body_mutexes = 0; // default
    constexpr JPH::uint max_body_pairs = 65536;
    constexpr JPH::uint max_contacts = 10240;
    m.system.Init(max_bodies, num_body_mutexes, max_body_pairs, max_contacts, m.bp_layer,
                  m.obj_vs_bp, m.obj_pair);
    m.system.SetGravity(JPH::Vec3(0.0F, -9.81F, 0.0F));

    m.contact_listener.events = &m.trigger_events;
    m.contact_listener.mutex = &m.trigger_mutex;
    m.contact_listener.sensors = &m.sensor_bodies;
    m.system.SetContactListener(&m.contact_listener);
    return world;
}

PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;
PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::update(float dt) {
    if (!impl_) return;
    {
        const std::scoped_lock lock(impl_->trigger_mutex);
        impl_->trigger_events.clear();
    }
    impl_->accumulator += dt;
    int steps = 0;
    // Clamp the number of catch-up steps so a long frame can't spiral.
    while (impl_->accumulator >= fixed_timestep && steps < max_steps_per_update) {
        impl_->system.Update(fixed_timestep, 1, impl_->temp_allocator.get(),
                             impl_->job_system.get());
        impl_->accumulator -= fixed_timestep;
        ++steps;
    }
    // Hitting the cap means the simulation fell behind real time (a hitch, a
    // blocking load, a slow machine). Drop the backlog, keeping only the
    // sub-step remainder: otherwise every following frame keeps stepping at
    // the cap and the world runs several times too fast until it drains.
    if (steps == max_steps_per_update) {
        impl_->accumulator = std::fmod(impl_->accumulator, fixed_timestep);
    }
}

float PhysicsWorld::pending_time() const { return impl_ ? impl_->accumulator : 0.0F; }

std::uint32_t PhysicsWorld::create_box(const glm::vec3& half_extents) {
    impl_->shapes.emplace_back(new JPH::BoxShape(to_jph(half_extents)));
    return static_cast<std::uint32_t>(impl_->shapes.size() - 1);
}

std::uint32_t PhysicsWorld::create_sphere(float radius) {
    impl_->shapes.emplace_back(new JPH::SphereShape(radius));
    return static_cast<std::uint32_t>(impl_->shapes.size() - 1);
}

std::uint32_t PhysicsWorld::create_capsule(float half_height, float radius) {
    impl_->shapes.emplace_back(new JPH::CapsuleShape(half_height, radius));
    return static_cast<std::uint32_t>(impl_->shapes.size() - 1);
}

std::uint32_t PhysicsWorld::create_convex(std::span<const glm::vec3> points) {
    JPH::Array<JPH::Vec3> jpoints;
    jpoints.reserve(points.size());
    for (const glm::vec3& p : points) {
        jpoints.push_back(to_jph(p));
    }
    JPH::ConvexHullShapeSettings settings(jpoints);
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        return invalid_body;
    }
    impl_->shapes.push_back(result.Get());
    return static_cast<std::uint32_t>(impl_->shapes.size() - 1);
}

std::uint32_t PhysicsWorld::create_mesh(std::span<const glm::vec3> points,
                                        std::span<const std::uint32_t> indices) {
    JPH::VertexList verts;
    verts.reserve(points.size());
    for (const glm::vec3& p : points) {
        verts.push_back(JPH::Float3(p.x, p.y, p.z));
    }
    JPH::IndexedTriangleList tris;
    tris.reserve(indices.size() / 3);
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        tris.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2], 0));
    }
    JPH::MeshShapeSettings settings(verts, tris);
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        return invalid_body;
    }
    impl_->shapes.push_back(result.Get());
    return static_cast<std::uint32_t>(impl_->shapes.size() - 1);
}

std::uint32_t PhysicsWorld::create_height_field(std::span<const float> samples,
                                                std::uint32_t sample_count,
                                                const glm::vec3& offset, const glm::vec3& scale) {
    if (sample_count < 2 ||
        samples.size() != static_cast<std::size_t>(sample_count) * sample_count) {
        return invalid_body;
    }
    JPH::HeightFieldShapeSettings settings(samples.data(),
                                           JPH::Vec3(offset.x, offset.y, offset.z),
                                           JPH::Vec3(scale.x, scale.y, scale.z), sample_count);
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        return invalid_body;
    }
    impl_->shapes.push_back(result.Get());
    return static_cast<std::uint32_t>(impl_->shapes.size() - 1);
}

std::uint32_t PhysicsWorld::add_body(const BodyParams& params) {
    if (params.shape >= impl_->shapes.size()) {
        return invalid_body;
    }
    const JPH::EMotionType motion = params.motion == Motion::static_body ? JPH::EMotionType::Static
                                    : params.motion == Motion::kinematic ? JPH::EMotionType::Kinematic
                                                                         : JPH::EMotionType::Dynamic;
    JPH::BodyCreationSettings settings(impl_->shapes[params.shape], to_jph_r(params.position),
                                       to_jph(params.rotation), motion,
                                       object_layer_of(params.layer));
    if (params.motion == Motion::dynamic && params.mass > 0.0F) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = params.mass;
    }
    settings.mIsSensor = params.sensor;
    const JPH::EActivation activation =
        params.motion == Motion::static_body ? JPH::EActivation::DontActivate
                                             : JPH::EActivation::Activate;
    const JPH::BodyID id =
        impl_->system.GetBodyInterface().CreateAndAddBody(settings, activation);
    if (id.IsInvalid()) {
        return invalid_body;
    }
    const std::uint32_t value = id.GetIndexAndSequenceNumber();
    if (params.owner != no_owner) impl_->body_owner[value] = params.owner;
    if (params.sensor) {
        impl_->sensor_bodies.insert(value);
    }
    return value;
}

void PhysicsWorld::remove_body(std::uint32_t body) {
    if (body == invalid_body) return;
    const JPH::BodyID id(body);
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    bodies.RemoveBody(id);
    bodies.DestroyBody(id);
    impl_->body_owner.erase(body);
    impl_->sensor_bodies.erase(body);
}

std::vector<PhysicsWorld::OwnedBody> PhysicsWorld::owned_bodies() const {
    std::vector<OwnedBody> out;
    out.reserve(impl_->body_owner.size());
    for (const auto& [body, owner] : impl_->body_owner) out.push_back({body, owner});
    return out;
}

void PhysicsWorld::body_transform(std::uint32_t body, glm::vec3& position,
                                  glm::quat& rotation) const {
    if (body == invalid_body) return;
    const JPH::BodyID id(body);
    const JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    position = to_glm(bodies.GetPosition(id));
    rotation = to_glm(bodies.GetRotation(id));
}

void PhysicsWorld::set_body_transform(std::uint32_t body, const glm::vec3& position,
                                      const glm::quat& rotation) {
    if (body == invalid_body) return;
    impl_->system.GetBodyInterface().SetPositionAndRotation(
        JPH::BodyID(body), to_jph_r(position), to_jph(rotation), JPH::EActivation::Activate);
}

std::uint32_t PhysicsWorld::create_character(float half_height, float radius,
                                             const glm::vec3& position, std::uint32_t owner) {
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = new JPH::CapsuleShape(half_height, radius);
    // Accept ground contacts within the bottom hemisphere as "supporting".
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);
    JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
        settings, to_jph_r(position), JPH::Quat::sIdentity(), 0, &impl_->system);
    impl_->characters.push_back(character);
    impl_->character_owner.push_back(owner);
    return static_cast<std::uint32_t>(impl_->characters.size() - 1);
}

void PhysicsWorld::remove_character(std::uint32_t character) {
    if (character >= impl_->characters.size()) return;
    impl_->characters[character] = nullptr; // releases the CharacterVirtual
    impl_->character_owner[character] = no_owner;
}

std::vector<PhysicsWorld::OwnedBody> PhysicsWorld::owned_characters() const {
    std::vector<OwnedBody> out;
    for (std::size_t i = 0; i < impl_->characters.size(); ++i) {
        if (impl_->characters[i] != nullptr && impl_->character_owner[i] != no_owner) {
            out.push_back({static_cast<std::uint32_t>(i), impl_->character_owner[i]});
        }
    }
    return out;
}

void PhysicsWorld::character_set_velocity(std::uint32_t character, const glm::vec3& velocity) {
    if (JPH::CharacterVirtual* c = impl_->character(character)) {
        c->SetLinearVelocity(to_jph(velocity));
    }
}

glm::vec3 PhysicsWorld::character_velocity(std::uint32_t character) const {
    const JPH::CharacterVirtual* c = impl_->character(character);
    return c != nullptr ? to_glm(c->GetLinearVelocity()) : glm::vec3(0.0F);
}

void PhysicsWorld::character_update(std::uint32_t character, float dt) {
    JPH::CharacterVirtual* c = impl_->character(character);
    if (c == nullptr) return;
    const JPH::ObjectLayer layer = object_layer_of(Layer::character);
    const JPH::DefaultBroadPhaseLayerFilter bp_filter(impl_->obj_vs_bp, layer);
    const JPH::DefaultObjectLayerFilter obj_filter(impl_->obj_pair, layer);
    JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;
    c->ExtendedUpdate(dt, impl_->system.GetGravity(), update_settings, bp_filter, obj_filter,
                      JPH::BodyFilter{}, JPH::ShapeFilter{}, *impl_->temp_allocator);
}

glm::vec3 PhysicsWorld::character_position(std::uint32_t character) const {
    const JPH::CharacterVirtual* c = impl_->character(character);
    return c != nullptr ? to_glm(c->GetPosition()) : glm::vec3(0.0F);
}

void PhysicsWorld::character_set_position(std::uint32_t character, const glm::vec3& position) {
    JPH::CharacterVirtual* c = impl_->character(character);
    if (c == nullptr) return;
    c->SetPosition(to_jph_r(position));
    // SetPosition keeps the contacts of the old location; re-detect them so
    // the ground state and the next update start from the new one.
    const JPH::ObjectLayer layer = object_layer_of(Layer::character);
    const JPH::DefaultBroadPhaseLayerFilter bp_filter(impl_->obj_vs_bp, layer);
    const JPH::DefaultObjectLayerFilter obj_filter(impl_->obj_pair, layer);
    c->RefreshContacts(bp_filter, obj_filter, JPH::BodyFilter{}, JPH::ShapeFilter{},
                       *impl_->temp_allocator);
}

bool PhysicsWorld::character_on_ground(std::uint32_t character) const {
    const JPH::CharacterVirtual* c = impl_->character(character);
    return c != nullptr && c->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

std::span<const PhysicsWorld::TriggerEvent> PhysicsWorld::trigger_events() const {
    return {impl_->trigger_events.data(), impl_->trigger_events.size()};
}

std::uint32_t PhysicsWorld::body_count() const {
    return impl_ ? impl_->system.GetNumBodies() : 0;
}

std::uint32_t PhysicsWorld::active_body_count() const {
    return impl_ ? impl_->system.GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0;
}

std::uint32_t PhysicsWorld::character_count() const {
    if (!impl_) return 0;
    std::uint32_t n = 0;
    for (const auto& c : impl_->characters) n += c != nullptr ? 1U : 0U;
    return n;
}

namespace {
// Skips sensor (trigger) bodies unless asked to include them.
class SensorBodyFilter final : public JPH::BodyFilter {
public:
    explicit SensorBodyFilter(bool include_sensors) : include_sensors_(include_sensors) {}
    [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override {
        return include_sensors_ || !body.IsSensor();
    }

private:
    bool include_sensors_;
};
} // namespace

std::optional<PhysicsWorld::RayHit> PhysicsWorld::cast_ray(const glm::vec3& origin,
                                                           const glm::vec3& direction,
                                                           float max_distance,
                                                           bool include_sensors) const {
    const float len = glm::length(direction);
    if (!impl_ || !(len > 1e-6F) || !(max_distance > 0.0F) || !std::isfinite(max_distance)) {
        return std::nullopt;
    }
    const glm::vec3 dir = direction / len;
    const JPH::RRayCast ray(to_jph_r(origin), to_jph(dir * max_distance));
    std::optional<RayHit> best;
    float best_fraction = 1.0F + FLT_EPSILON;

    JPH::RayCastResult result;
    const SensorBodyFilter filter(include_sensors);
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
        if (c == nullptr) continue;
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
