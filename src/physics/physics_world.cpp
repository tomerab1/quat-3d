#include "engine/physics/physics_world.hpp"

#include <cmath>
#include <thread>

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
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
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

} // namespace

struct PhysicsWorld::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl>   temp_allocator;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system;
    BroadPhaseLayerImpl                       bp_layer;
    ObjectVsBroadPhaseFilterImpl              obj_vs_bp;
    ObjectLayerPairFilterImpl                 obj_pair;
    JPH::PhysicsSystem                        system;
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
