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
#include <span>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/core/error.hpp"

namespace engine::physics {

// Object collision layers (mirrors the values used inside the Jolt wrapper).
enum class Layer : std::uint16_t { static_body = 0, dynamic = 1, character = 2, trigger = 3, sensor = 4 };

// How a body moves: never (static), driven by the engine (kinematic), or by the
// simulation (dynamic).
enum class Motion : std::uint8_t { static_body, kinematic, dynamic };

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

    // Sentinel for "no body".
    static constexpr std::uint32_t invalid_body = 0xFFFFFFFFU;

    // ---- Collision shape factories: return an opaque shape id owned by the
    // world (Jolt ref-counts; the id is reusable across many bodies). -----------
    [[nodiscard]] std::uint32_t create_box(const glm::vec3& half_extents);
    [[nodiscard]] std::uint32_t create_sphere(float radius);
    [[nodiscard]] std::uint32_t create_capsule(float half_height, float radius);
    [[nodiscard]] std::uint32_t create_convex(std::span<const glm::vec3> points);
    [[nodiscard]] std::uint32_t create_mesh(std::span<const glm::vec3> points,
                                            std::span<const std::uint32_t> indices);

    struct BodyParams {
        std::uint32_t shape = 0;
        glm::vec3     position{0.0F};
        glm::quat     rotation{1.0F, 0.0F, 0.0F, 0.0F};
        Motion        motion = Motion::dynamic;
        Layer         layer = Layer::dynamic;
        float         mass = 1.0F; // dynamic only; <= 0 uses the shape's computed mass
    };

    // Create a body and add it to the simulation. Returns its id (invalid_body on
    // failure / an out-of-range shape).
    [[nodiscard]] std::uint32_t add_body(const BodyParams& params);
    void remove_body(std::uint32_t body);

    // Read / write a body's world transform. set_* is for kinematic bodies.
    void body_transform(std::uint32_t body, glm::vec3& position, glm::quat& rotation) const;
    void set_body_transform(std::uint32_t body, const glm::vec3& position,
                            const glm::quat& rotation);

    // ---- Character controller (predictive, JPH::CharacterVirtual). ------------
    // A capsule character: full cylinder half-height + radius, at `position`.
    [[nodiscard]] std::uint32_t create_character(float half_height, float radius,
                                                 const glm::vec3& position);
    // The caller integrates gravity into the velocity; update moves + resolves
    // collisions (with stair step-up) for `dt`.
    void character_set_velocity(std::uint32_t character, const glm::vec3& velocity);
    [[nodiscard]] glm::vec3 character_velocity(std::uint32_t character) const;
    void character_update(std::uint32_t character, float dt);
    [[nodiscard]] glm::vec3 character_position(std::uint32_t character) const;
    [[nodiscard]] bool character_on_ground(std::uint32_t character) const;

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
