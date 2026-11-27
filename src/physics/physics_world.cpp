// PhysicsWorld: world lifetime and stepping, shapes and bodies. Characters
// live in physics_character.cpp, queries and counters in physics_queries.cpp;
// the shared Jolt state is physics_world_impl.hpp.

#include "engine/physics/physics_world.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

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

[[nodiscard]] JPH::ShapeRefC built(const JPH::ShapeSettings& settings) {
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    return result.HasError() ? JPH::ShapeRefC{} : JPH::ShapeRefC(result.Get());
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
    m.job_system = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs,
                                                              JPH::cMaxPhysicsBarriers, threads);

    constexpr JPH::uint max_bodies = 65536;
    constexpr JPH::uint num_body_mutexes = 0; // default
    constexpr JPH::uint max_body_pairs = 65536;
    constexpr JPH::uint max_contacts = 10240;
    m.system.Init(max_bodies, num_body_mutexes, max_body_pairs, max_contacts, m.bp_layer,
                  m.obj_vs_bp, m.obj_pair);
    m.system.SetGravity(JPH::Vec3(0.0F, -9.81F, 0.0F));

    m.contact_listener.buffers = &m.contact_buffers;
    m.contact_listener.sensors = &m.sensor_bodies;
    m.system.SetContactListener(&m.contact_listener);
    return world;
}

PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;
PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::update(float dt) {
    if (!impl_) return;
    Impl& m = *impl_;
    // Contacts ended by remove_body() since the last update open the list.
    m.contact_events.clear();
    m.contact_events.swap(m.pending_events);
    m.dropped_contacts = 0;
    m.accumulator += dt;
    int steps = 0;
    // Clamp the number of catch-up steps so a long frame can't spiral.
    while (m.accumulator >= fixed_timestep && steps < max_steps_per_update) {
        m.apply_forces(fixed_timestep); // Jolt clears them after every step
        m.system.Update(fixed_timestep, 1, m.temp_allocator.get(), m.job_system.get());
        m.accumulator -= fixed_timestep;
        ++steps;
        ++m.steps;
        // The step's workers are done: take their buffer (the next step
        // writes into the other one) and resolve it into body-pair events.
        const ContactBuffer& done = m.contact_buffers.swap();
        m.contact_tracker.process(done.contacts(), m.steps, *this,
                                  m.system.GetBodyInterfaceNoLock(), m.contact_events);
        m.dropped_contacts += done.dropped();
    }
    m.trigger_events.clear();
    for (const ContactEvent& ev : m.contact_events) {
        if (ev.kind == ContactEvent::Kind::trigger_enter ||
            ev.kind == ContactEvent::Kind::trigger_exit) {
            m.trigger_events.push_back(
                {ev.body_a, ev.body_b, ev.kind == ContactEvent::Kind::trigger_enter});
        }
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

void PhysicsWorld::set_gravity(const glm::vec3& gravity) {
    if (!impl_) return;
    const JPH::Vec3 g = to_jph(gravity);
    if (impl_->system.GetGravity() == g) return;
    impl_->system.SetGravity(g);
    // Sleeping bodies would ignore the change until something touched them.
    JPH::BodyIDVector ids;
    impl_->system.GetBodies(ids);
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [&](const JPH::BodyID& id) {
                                 return bi.GetMotionType(id) != JPH::EMotionType::Dynamic;
                             }),
              ids.end());
    if (!ids.empty()) bi.ActivateBodies(ids.data(), static_cast<int>(ids.size()));
}

glm::vec3 PhysicsWorld::gravity() const {
    return impl_ ? to_glm(impl_->system.GetGravity()) : glm::vec3(0.0F, -9.81F, 0.0F);
}

// ---- Shapes -----------------------------------------------------------------

std::uint32_t PhysicsWorld::create_box(const glm::vec3& half_extents) {
    if (!glm::all(glm::greaterThan(half_extents, glm::vec3(0.0F)))) return invalid_body;
    return impl_->add_shape(new JPH::BoxShape(
        to_jph(half_extents), std::min({half_extents.x, half_extents.y, half_extents.z,
                                        JPH::cDefaultConvexRadius})));
}

std::uint32_t PhysicsWorld::create_sphere(float radius) {
    if (!(radius > 0.0F)) return invalid_body;
    return impl_->add_shape(new JPH::SphereShape(radius));
}

std::uint32_t PhysicsWorld::create_capsule(float half_height, float radius) {
    if (!(radius > 0.0F) || !(half_height >= 0.0F)) return invalid_body;
    return impl_->add_shape(new JPH::CapsuleShape(half_height, radius));
}

std::uint32_t PhysicsWorld::create_convex(std::span<const glm::vec3> points) {
    JPH::Array<JPH::Vec3> jpoints;
    jpoints.reserve(points.size());
    for (const glm::vec3& p : points) {
        jpoints.push_back(to_jph(p));
    }
    return impl_->add_shape(built(JPH::ConvexHullShapeSettings(jpoints)));
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
    return impl_->add_shape(built(JPH::MeshShapeSettings(verts, tris)));
}

std::uint32_t PhysicsWorld::create_height_field(std::span<const float> samples,
                                                std::uint32_t sample_count,
                                                const glm::vec3& offset, const glm::vec3& scale) {
    if (sample_count < 2 ||
        samples.size() != static_cast<std::size_t>(sample_count) * sample_count) {
        return invalid_body;
    }
    return impl_->add_shape(built(JPH::HeightFieldShapeSettings(
        samples.data(), JPH::Vec3(offset.x, offset.y, offset.z),
        JPH::Vec3(scale.x, scale.y, scale.z), sample_count)));
}

void PhysicsWorld::release_shape(std::uint32_t shape) {
    if (!impl_) return;
    const ShapeRecord* rec = impl_->shapes.get(shape);
    if (rec != nullptr && rec->bodies == 0) impl_->shapes.erase(shape);
}

std::uint32_t PhysicsWorld::shape_count() const {
    return impl_ ? static_cast<std::uint32_t>(impl_->shapes.size()) : 0;
}

// ---- Bodies -----------------------------------------------------------------

std::uint32_t PhysicsWorld::add_body(const BodyParams& params) {
    ShapeRecord* shape = impl_->shapes.get(params.shape);
    if (shape == nullptr) {
        return invalid_body;
    }
    const JPH::EMotionType motion = params.motion == Motion::static_body ? JPH::EMotionType::Static
                                    : params.motion == Motion::kinematic ? JPH::EMotionType::Kinematic
                                                                         : JPH::EMotionType::Dynamic;
    JPH::BodyCreationSettings settings(shape->shape, to_jph_r(params.position),
                                       to_jph(params.rotation), motion,
                                       object_layer_of(params.layer));
    if (params.motion == Motion::dynamic && params.mass > 0.0F) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = params.mass;
    }
    settings.mIsSensor = params.sensor;
    settings.mFriction = params.friction;
    settings.mRestitution = params.restitution;
    settings.mGravityFactor = params.gravity_factor;
    settings.mLinearDamping = params.linear_damping;
    settings.mAngularDamping = params.angular_damping;
    // Moving bodies can switch between dynamic and kinematic at runtime
    // (set_motion); static ones skip the motion state that would need.
    settings.mAllowDynamicOrKinematic = params.motion != Motion::static_body;
    const JPH::EActivation activation =
        params.motion == Motion::static_body ? JPH::EActivation::DontActivate
                                             : JPH::EActivation::Activate;
    const JPH::BodyID id =
        impl_->system.GetBodyInterface().CreateAndAddBody(settings, activation);
    if (id.IsInvalid()) {
        return invalid_body;
    }
    const std::uint32_t value = id.GetIndexAndSequenceNumber();
    ++shape->bodies;
    impl_->bodies[value] = BodyRecord{params.owner, params.shape, params.sensor};
    if (params.sensor) {
        impl_->sensor_bodies.insert(value);
    }
    return value;
}

void PhysicsWorld::remove_body(std::uint32_t body) {
    if (!impl_ || body == invalid_body) return;
    const auto it = impl_->bodies.find(body);
    if (it == impl_->bodies.end()) return; // never ours, or already removed
    const JPH::BodyID id(body);
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    bodies.RemoveBody(id);
    bodies.DestroyBody(id);
    const std::uint32_t shape = it->second.shape;
    impl_->bodies.erase(it);
    // Jolt reports the end of the body's awake contacts in the next update;
    // the sleeping ones it no longer tracks end here.
    impl_->contact_tracker.body_removed(body, impl_->steps, impl_->pending_events);
    impl_->sensor_bodies.erase(body);
    impl_->unref_shape(shape); // after DestroyBody: Jolt's own reference is gone too
}

std::vector<PhysicsWorld::OwnedBody> PhysicsWorld::owned_bodies() const {
    std::vector<OwnedBody> out;
    out.reserve(impl_->bodies.size());
    for (const auto& [body, rec] : impl_->bodies) {
        if (rec.owner != no_owner) out.push_back({body, rec.owner});
    }
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

} // namespace engine::physics
