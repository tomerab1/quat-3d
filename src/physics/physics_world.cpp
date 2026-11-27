#include "engine/physics/physics_world.hpp"

#include <cfloat>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/core/profile.hpp"
#include "physics_world_impl.hpp"

namespace engine::physics {

using namespace detail;

namespace {

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

} // namespace

std::expected<PhysicsWorld, core::Error> PhysicsWorld::create() {
    ensure_jolt_initialized();

    PhysicsWorld world;
    world.impl_ = std::make_unique<Impl>();
    Impl& m = *world.impl_;

    m.temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
    const unsigned hw = std::thread::hardware_concurrency();
    const int threads = static_cast<int>(hw > 1 ? hw - 1 : 1);
    // Jolt's workers are named for the profiler (R4) before they start.
    m.job_system = std::make_unique<JPH::JobSystemThreadPool>();
    m.job_system->SetThreadInitFunction([](int index) {
        const std::string name = "physics worker " + std::to_string(index);
        QUAT_THREAD_NAME(name.c_str());
    });
    m.job_system->Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);

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
    QUAT_ZONE_N("physics_step");
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
    // A dt over the budget means the simulation fell behind real time (a
    // hitch, a blocking load, a slow machine). Drop the backlog, keeping only
    // the sub-step remainder: otherwise every following frame keeps stepping
    // at the cap and the world runs several times too fast until it drains.
    // Within the budget nothing is dropped — the carried remainder plus a
    // full-budget dt can leave just over one tick (float rounding), which the
    // next call runs (audit D2: sliced debug time must be simulated exactly).
    if (dt > max_update_dt) {
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
    // Inner body: a kinematic capsule that follows the character so sensors
    // report it (trigger events name it; body_owner() maps it back).
    settings->mInnerBodyShape = settings->mShape;
    settings->mInnerBodyLayer = obj_layers::k_character_presence;
    JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
        settings, to_jph_r(position), JPH::Quat::sIdentity(), 0, &impl_->system);
    const auto index = static_cast<std::uint32_t>(impl_->characters.size());
    if (const JPH::BodyID inner = character->GetInnerBodyID(); !inner.IsInvalid()) {
        impl_->inner_body_character[inner.GetIndexAndSequenceNumber()] = index;
    }
    impl_->characters.push_back(character);
    impl_->character_owner.push_back(owner);
    return index;
}

void PhysicsWorld::remove_character(std::uint32_t character) {
    if (character >= impl_->characters.size()) return;
    if (const JPH::CharacterVirtual* c = impl_->characters[character].GetPtr()) {
        impl_->inner_body_character.erase(c->GetInnerBodyID().GetIndexAndSequenceNumber());
    }
    impl_->characters[character] = nullptr; // releases the CharacterVirtual (and its inner body)
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

} // namespace engine::physics
