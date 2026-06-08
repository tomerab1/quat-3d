#include "engine/physics/physics_world.hpp"

#include <cmath>
#include <thread>
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
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

using namespace JPH::literals;

namespace engine::physics {

namespace {

// Object layers (must match the Layer enum in the header).
namespace obj_layers {
constexpr JPH::ObjectLayer k_static = 0;
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

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
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
    float                                     accumulator = 0.0F;
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
    return world;
}

PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;
PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::update(float dt) {
    if (!impl_) return;
    impl_->accumulator += dt;
    int steps = 0;
    // Clamp the number of catch-up steps so a long frame can't spiral.
    while (impl_->accumulator >= fixed_timestep && steps < 8) {
        impl_->system.Update(fixed_timestep, 1, impl_->temp_allocator.get(),
                             impl_->job_system.get());
        impl_->accumulator -= fixed_timestep;
        ++steps;
    }
}

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
    const JPH::EActivation activation =
        params.motion == Motion::static_body ? JPH::EActivation::DontActivate
                                             : JPH::EActivation::Activate;
    const JPH::BodyID id =
        impl_->system.GetBodyInterface().CreateAndAddBody(settings, activation);
    return id.IsInvalid() ? invalid_body : id.GetIndexAndSequenceNumber();
}

void PhysicsWorld::remove_body(std::uint32_t body) {
    if (body == invalid_body) return;
    const JPH::BodyID id(body);
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    bodies.RemoveBody(id);
    bodies.DestroyBody(id);
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
                                             const glm::vec3& position) {
    JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
    settings->mShape = new JPH::CapsuleShape(half_height, radius);
    // Accept ground contacts within the bottom hemisphere as "supporting".
    settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);
    JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
        settings, to_jph_r(position), JPH::Quat::sIdentity(), 0, &impl_->system);
    impl_->characters.push_back(character);
    return static_cast<std::uint32_t>(impl_->characters.size() - 1);
}

void PhysicsWorld::character_set_velocity(std::uint32_t character, const glm::vec3& velocity) {
    if (character >= impl_->characters.size()) return;
    impl_->characters[character]->SetLinearVelocity(to_jph(velocity));
}

glm::vec3 PhysicsWorld::character_velocity(std::uint32_t character) const {
    if (character >= impl_->characters.size()) return glm::vec3(0.0F);
    return to_glm(impl_->characters[character]->GetLinearVelocity());
}

void PhysicsWorld::character_update(std::uint32_t character, float dt) {
    if (character >= impl_->characters.size()) return;
    const JPH::ObjectLayer layer = object_layer_of(Layer::character);
    const JPH::DefaultBroadPhaseLayerFilter bp_filter(impl_->obj_vs_bp, layer);
    const JPH::DefaultObjectLayerFilter obj_filter(impl_->obj_pair, layer);
    JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;
    impl_->characters[character]->ExtendedUpdate(dt, impl_->system.GetGravity(), update_settings,
                                                 bp_filter, obj_filter, JPH::BodyFilter{},
                                                 JPH::ShapeFilter{}, *impl_->temp_allocator);
}

glm::vec3 PhysicsWorld::character_position(std::uint32_t character) const {
    if (character >= impl_->characters.size()) return glm::vec3(0.0F);
    return to_glm(impl_->characters[character]->GetPosition());
}

bool PhysicsWorld::character_on_ground(std::uint32_t character) const {
    if (character >= impl_->characters.size()) return false;
    return impl_->characters[character]->GetGroundState() ==
           JPH::CharacterBase::EGroundState::OnGround;
}

std::expected<void, core::Error> run_physics_self_test() {
    auto world = PhysicsWorld::create();
    if (!world) return std::unexpected(world.error());
    JPH::PhysicsSystem& system = world->impl_->system;
    JPH::BodyInterface& bodies = system.GetBodyInterface();

    // Static floor: a 100x2x100 box centred at y=-1, so its top is at y=0.
    JPH::BodyCreationSettings floor_settings(new JPH::BoxShape(JPH::Vec3(50.0F, 1.0F, 50.0F)),
                                             JPH::RVec3(0.0_r, -1.0_r, 0.0_r),
                                             JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                             obj_layers::k_static);
    const JPH::BodyID floor = bodies.CreateAndAddBody(floor_settings, JPH::EActivation::DontActivate);

    // Dynamic sphere (radius 0.5) dropped from y=5.
    JPH::BodyCreationSettings sphere_settings(new JPH::SphereShape(0.5F),
                                              JPH::RVec3(0.0_r, 5.0_r, 0.0_r),
                                              JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic,
                                              obj_layers::k_dynamic);
    const JPH::BodyID sphere = bodies.CreateAndAddBody(sphere_settings, JPH::EActivation::Activate);

    system.OptimizeBroadPhase();

    // Simulate ~2 seconds at 60 fps; the sphere should land and settle.
    for (int i = 0; i < 120; ++i) {
        world->update(1.0F / 60.0F);
    }

    const float y = static_cast<float>(bodies.GetPosition(sphere).GetY());

    bodies.RemoveBody(sphere);
    bodies.DestroyBody(sphere);
    bodies.RemoveBody(floor);
    bodies.DestroyBody(floor);

    if (y > 4.5F) return fail("physics self-test: sphere did not fall");
    if (y < 0.0F) return fail("physics self-test: sphere fell through the floor");
    if (std::abs(y - 0.5F) > 0.25F) {
        return fail("physics self-test: sphere did not come to rest on the floor");
    }
    return {};
}

} // namespace engine::physics
