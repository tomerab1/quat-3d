#pragma once

// PhysicsWorld — Jolt Physics integration (Phase 6, Slice 6.1).
//
// Owns a JPH::PhysicsSystem plus its temp allocator, job system, and collision
// layer/broad-phase filters, and steps it on a fixed 1/120 s accumulator. Jolt
// types are kept out of this header (PIMPL); body creation and ECS sync arrive in
// 6.2. Collision object layers: STATIC, DYNAMIC, CHARACTER, TRIGGER, SENSOR.

#include <cstdint>
#include <expected>
#include <memory>

#include "engine/core/error.hpp"

namespace engine::physics {

// Object collision layers (mirrors the values used inside the Jolt wrapper).
enum class Layer : std::uint16_t { static_body = 0, dynamic = 1, character = 2, trigger = 3, sensor = 4 };

class PhysicsWorld {
public:
    [[nodiscard]] static std::expected<PhysicsWorld, core::Error> create();

    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    ~PhysicsWorld();

    // Advance by `dt` seconds, stepping the simulation in fixed 1/120 s ticks
    // (accumulated; clamped to avoid a spiral of death on long frames).
    void update(float dt);

    // Fixed simulation timestep.
    static constexpr float fixed_timestep = 1.0F / 120.0F;

private:
    PhysicsWorld() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend std::expected<void, core::Error> run_physics_self_test();
};

// Creates a world with a static floor and a dynamic sphere dropped from above,
// steps ~2 s, and verifies the sphere fell and came to rest on the floor.
[[nodiscard]] std::expected<void, core::Error> run_physics_self_test();

} // namespace engine::physics
