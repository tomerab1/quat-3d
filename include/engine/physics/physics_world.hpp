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
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/core/error.hpp"

namespace engine::physics {

// Object collision layers (mirrors the values used inside the Jolt wrapper).
enum class Layer : std::uint16_t { static_body = 0, dynamic = 1, character = 2, trigger = 3, sensor = 4 };

// A set of object layers (query filters): bit (1 << layer) per layer.
using LayerMask = std::uint32_t;
[[nodiscard]] constexpr LayerMask layer_bit(Layer layer) {
    return LayerMask{1} << static_cast<unsigned>(layer);
}
inline constexpr LayerMask all_layers = 0x1FU;
// What queries hit unless told otherwise: solid things, not trigger volumes.
inline constexpr LayerMask solid_layers =
    layer_bit(Layer::static_body) | layer_bit(Layer::dynamic) | layer_bit(Layer::character);

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
    // Catch-up limit per update(); time beyond it is dropped, not carried.
    static constexpr int max_steps_per_update = 8;

    // Accumulated time not yet simulated (always < fixed_timestep after a
    // capped update). Render interpolation and debugging read it.
    [[nodiscard]] float pending_time() const;

    // World gravity, m/s^2 (default (0, -9.81, 0)). Changing it wakes every
    // moving body so resting ones feel it; character_update uses it too.
    void set_gravity(const glm::vec3& gravity);
    [[nodiscard]] glm::vec3 gravity() const;

    // Sentinel for "no body".
    static constexpr std::uint32_t invalid_body = 0xFFFFFFFFU;

    // ---- Collision shape factories ---------------------------------------
    // Each returns a shape handle (invalid_body on bad input). A shape may back
    // any number of bodies; it is reference-counted by them and released when
    // the last body using it is removed, so streamed terrain tiles and rebuilt
    // colliders do not accumulate (issue #47). A shape that never backs a body
    // must be given back with release_shape(). Handles carry a generation:
    // a released handle never resolves to a later shape in the same slot.
    [[nodiscard]] std::uint32_t create_box(const glm::vec3& half_extents);
    [[nodiscard]] std::uint32_t create_sphere(float radius);
    [[nodiscard]] std::uint32_t create_capsule(float half_height, float radius);
    [[nodiscard]] std::uint32_t create_convex(std::span<const glm::vec3> points);
    [[nodiscard]] std::uint32_t create_mesh(std::span<const glm::vec3> points,
                                            std::span<const std::uint32_t> indices);
    // Static terrain surface: sample (x, z) sits at offset + scale * (x, h, z)
    // with h = samples[z * sample_count + x]. sample_count must be a multiple
    // of Jolt's block size (2) — callers pad odd grids by duplicating an edge.
    [[nodiscard]] std::uint32_t create_height_field(std::span<const float> samples,
                                                    std::uint32_t sample_count,
                                                    const glm::vec3& offset,
                                                    const glm::vec3& scale);

    struct BodyParams {
        std::uint32_t shape = 0;
        glm::vec3     position{0.0F};
        glm::quat     rotation{1.0F, 0.0F, 0.0F, 0.0F};
        Motion        motion = Motion::dynamic;
        Layer         layer = Layer::dynamic;
        float         mass = 1.0F; // dynamic only; <= 0 uses the shape's computed mass
        bool          sensor = false; // a trigger volume: detects overlaps, no physical response
        // Opaque id of whatever owns the body (the ECS passes its entity id).
        // Owned bodies are listed by owned_bodies() so the owner can sweep
        // away bodies whose owner no longer exists; no_owner bodies (terrain
        // height fields, demo floors) are never listed.
        std::uint32_t owner = no_owner;
        // Surface and motion tuning (Jolt's defaults). Damping and the
        // gravity factor only affect moving bodies.
        float friction = 0.2F;
        float restitution = 0.0F;
        float gravity_factor = 1.0F;
        float linear_damping = 0.05F;
        float angular_damping = 0.05F;
    };
    static constexpr std::uint32_t no_owner = 0xFFFFFFFFU;

    // A trigger overlap event, recorded by the contact listener during update().
    struct TriggerEvent {
        std::uint32_t trigger = invalid_body; // the sensor body
        std::uint32_t other = invalid_body;   // the body that entered / left it
        bool          entered = true;         // true = began overlapping, false = stopped
    };

    // Create a body and add it to the simulation. Returns its id (invalid_body on
    // failure / an unknown shape handle). The body holds a reference to its
    // shape until remove_body().
    [[nodiscard]] std::uint32_t add_body(const BodyParams& params);
    void remove_body(std::uint32_t body);

    // Releases a shape no body uses (one whose add_body failed, or that was
    // never attached). Shapes in use are left alone: their bodies own them.
    void release_shape(std::uint32_t shape);
    // Live collision shapes (created and not yet released).
    [[nodiscard]] std::uint32_t shape_count() const;

    // (body, owner) for every live body created with an owner.
    struct OwnedBody {
        std::uint32_t body = invalid_body;
        std::uint32_t owner = no_owner;
    };
    [[nodiscard]] std::vector<OwnedBody> owned_bodies() const;

    // Read / write a body's world transform. set_* is for kinematic bodies.
    void body_transform(std::uint32_t body, glm::vec3& position, glm::quat& rotation) const;
    void set_body_transform(std::uint32_t body, const glm::vec3& position,
                            const glm::quat& rotation);

    // ---- Character controller (predictive, JPH::CharacterVirtual). ------------
    // A capsule character: full cylinder half-height + radius, at `position`.
    // `owner` works as for bodies (see owned_characters()). Characters collide
    // with each other, and carry a kinematic inner body on the CHARACTER layer
    // that follows them, so dynamic bodies rest on / bounce off them and
    // sensors detect them (issue #48).
    [[nodiscard]] std::uint32_t create_character(float half_height, float radius,
                                                 const glm::vec3& position,
                                                 std::uint32_t owner = no_owner);
    // Removes the character from the world and frees its slot for reuse; the
    // handle goes stale (generation), so every character_* call on it becomes
    // a no-op.
    void remove_character(std::uint32_t character);
    // Whether `character` is a live character handle.
    [[nodiscard]] bool has_character(std::uint32_t character) const;
    // (character, owner) for every live character created with an owner.
    [[nodiscard]] std::vector<OwnedBody> owned_characters() const;
    // The caller integrates gravity into the velocity; update moves + resolves
    // collisions (with stair step-up) for `dt`.
    void character_set_velocity(std::uint32_t character, const glm::vec3& velocity);
    [[nodiscard]] glm::vec3 character_velocity(std::uint32_t character) const;
    void character_update(std::uint32_t character, float dt);
    [[nodiscard]] glm::vec3 character_position(std::uint32_t character) const;
    // Teleport the character (network reconciliation, respawn) and refresh its
    // contacts so on_ground / the next update see the new surroundings.
    void character_set_position(std::uint32_t character, const glm::vec3& position);
    [[nodiscard]] bool character_on_ground(std::uint32_t character) const;

    // Trigger (sensor) overlap events recorded during the most recent update().
    // Valid until the next update() call. A character takes part through its
    // inner body; body_owner() maps that to the character's owner.
    [[nodiscard]] std::span<const TriggerEvent> trigger_events() const;

    // The owner a body was created with (a character's owner for its inner
    // body); no_owner for unowned or unknown bodies.
    [[nodiscard]] std::uint32_t body_owner(std::uint32_t body) const;

    // ---- Contact events (A10.1) ------------------------------------------------
    // One event per body pair, not per touching sub-shape: contact_added when
    // two bodies start touching, contact_removed when they stop or one of them
    // is removed. A body falling asleep does not end its contacts (Jolt drops
    // them; the world waits until the body wakes up and shows whether it still
    // touches). trigger_enter / trigger_exit are the same for a sensor and a
    // body, with the sensor as `body_a`. Collected lock-free on Jolt's worker
    // threads into per-step buffers (at most max_contacts_per_step callbacks a
    // step; the rest are counted as dropped) and resolved on the calling thread
    // after each step.
    struct ContactEvent {
        enum class Kind : std::uint8_t { contact_added, contact_removed, trigger_enter, trigger_exit };
        Kind          kind = Kind::contact_added;
        std::uint32_t body_a = invalid_body;
        std::uint32_t body_b = invalid_body;
        std::uint32_t owner_a = no_owner; // body_owner() of each, cached when they met
        std::uint32_t owner_b = no_owner;
        glm::vec3     point{0.0F};    // world space (contact_added, trigger_enter)
        glm::vec3     normal{0.0F};   // from a towards b (contact_added)
        float         impulse = 0.0F; // estimated impact impulse, N*s (contact_added)
        std::uint64_t step = 0;       // step_count() when it happened
    };
    static constexpr std::uint32_t max_contacts_per_step = 4096;

    // Events of the most recent update() (plus contacts ended by remove_body
    // since), in order. Valid until the next update().
    [[nodiscard]] std::span<const ContactEvent> contact_events() const;
    // Contact callbacks the most recent update() could not record.
    [[nodiscard]] std::uint32_t dropped_contact_events() const;
    // Fixed steps simulated since create(), and the simulated seconds they make.
    [[nodiscard]] std::uint64_t step_count() const;
    [[nodiscard]] double        simulated_time() const;

    // ---- Body control (A10.1 remote, G7 scripts) ------------------------------
    // Runtime state of a live body; nullopt for an unknown / removed body.
    struct BodyState {
        Motion    motion = Motion::static_body;
        Layer     layer = Layer::static_body;
        bool      sensor = false;
        bool      awake = false;
        float     mass = 0.0F; // kg; 0 for static and kinematic bodies
        glm::vec3 linear_velocity{0.0F};  // m/s
        glm::vec3 angular_velocity{0.0F}; // rad/s about world axes
        glm::vec3 position{0.0F};         // centre of the body (collider offset included)
        glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
        float     friction = 0.0F;
        float     restitution = 0.0F;
        float     gravity_factor = 1.0F;  // moving bodies
        float     linear_damping = 0.0F;  // moving bodies
        float     angular_damping = 0.0F; // moving bodies
    };
    [[nodiscard]] std::optional<BodyState> body_state(std::uint32_t body) const;

    // The calls below are no-ops unless `body` is a live dynamic body.
    // Impulses (N*s) change the velocity at once and wake the body; a force
    // (N) or torque (N*m) acts on every simulation step for `duration_s` of
    // simulated time (Jolt clears forces after each step, so the world keeps
    // re-applying them), clamped to max_force_duration.
    static constexpr float max_force_duration = 10.0F;
    [[nodiscard]] bool is_dynamic(std::uint32_t body) const;
    void add_impulse(std::uint32_t body, const glm::vec3& impulse);
    void add_impulse(std::uint32_t body, const glm::vec3& impulse, const glm::vec3& world_point);
    void add_angular_impulse(std::uint32_t body, const glm::vec3& impulse);
    void add_force(std::uint32_t body, const glm::vec3& force, float duration_s);
    void add_force(std::uint32_t body, const glm::vec3& force, const glm::vec3& world_point,
                   float duration_s);
    void add_torque(std::uint32_t body, const glm::vec3& torque, float duration_s);
    void set_linear_velocity(std::uint32_t body, const glm::vec3& velocity);
    [[nodiscard]] glm::vec3 linear_velocity(std::uint32_t body) const;
    void set_angular_velocity(std::uint32_t body, const glm::vec3& radians_per_s);
    [[nodiscard]] glm::vec3 angular_velocity(std::uint32_t body) const;

    // Switches a body between dynamic and kinematic (any body not created
    // static). False when the body is static or unknown, or `motion` is static.
    bool set_motion(std::uint32_t body, Motion motion);
    // Surface / motion tuning of any live body (damping and the gravity
    // factor are ignored by static bodies).
    void set_friction(std::uint32_t body, float friction);
    void set_restitution(std::uint32_t body, float restitution);
    void set_gravity_factor(std::uint32_t body, float factor);
    void set_damping(std::uint32_t body, float linear, float angular);
    // Wakes a body up, or puts it to sleep.
    void set_awake(std::uint32_t body, bool awake);

    // ---- Joints (A10.1; Jolt two-body constraints) -----------------------------
    // Geometry is given in world space at creation. body_b invalid_body joins
    // body_a to the world. Removing either body removes its joints (the
    // handle goes stale: has_joint() turns false).
    enum class JointKind : std::uint8_t { fixed, point, hinge, slider, distance };
    enum class MotorMode : std::uint8_t { off, velocity, position };
    struct JointParams {
        JointKind     kind = JointKind::hinge;
        std::uint32_t body_a = invalid_body;
        std::uint32_t body_b = invalid_body;
        glm::vec3     point_a{0.0F};  // the pivot / anchor on body a
        glm::vec3     point_b{0.0F};  // distance: the other end (others use point_a)
        glm::vec3     axis{0.0F, 1.0F, 0.0F}; // hinge / slider, normalized
        bool          limited = false;
        float         limit_min = 0.0F; // hinge: radians in [-pi, 0]; slider: metres <= 0;
        float         limit_max = 0.0F; // distance: metres >= 0
        MotorMode     motor = MotorMode::off;
        float         motor_target = 0.0F;    // rad/s, rad, m/s or m
        float         motor_max_force = 0.0F; // N*m / N; 0 = unlimited
        std::uint32_t owner = no_owner;
    };
    // invalid_body when a body is unknown, the bodies are the same, or
    // neither is dynamic.
    [[nodiscard]] std::uint32_t add_joint(const JointParams& params);
    void remove_joint(std::uint32_t joint);
    [[nodiscard]] bool has_joint(std::uint32_t joint) const;
    // (joint, owner) for every live joint created with an owner.
    [[nodiscard]] std::vector<OwnedBody> owned_joints() const;
    [[nodiscard]] std::uint32_t joint_count() const;
    // Hinge: angle (radians) from the pose it was built in; slider: position
    // along the axis (m); distance: current distance (m). nullopt otherwise.
    [[nodiscard]] std::optional<float> joint_position(std::uint32_t joint) const;

    // ---- Queries (debugging, gameplay) -------------------------------------
    // Bodies in the simulation (all kinds, terrain included; not the
    // characters' inner bodies) and how many of them are awake (active rigid
    // bodies).
    [[nodiscard]] std::uint32_t body_count() const;
    [[nodiscard]] std::uint32_t active_body_count() const;
    // Live character controllers.
    [[nodiscard]] std::uint32_t character_count() const;

    struct RayHit {
        std::uint32_t body = invalid_body;      // set when a body was hit
        std::uint32_t character = invalid_body; // set when a character was hit
        std::uint32_t owner = no_owner;         // the hit body's / character's owner
        glm::vec3     point{0.0F};
        glm::vec3     normal{0.0F, 1.0F, 0.0F}; // world-space surface normal
        float         distance = 0.0F;          // from the ray origin, metres
        bool          sensor = false;
    };
    // Closest hit along `direction` (need not be normalized; zero = no hit)
    // within `max_distance`, against bodies (sensors only when
    // `include_sensors`) and character capsules. nullopt when nothing is hit.
    [[nodiscard]] std::optional<RayHit> cast_ray(const glm::vec3& origin, const glm::vec3& direction,
                                                 float max_distance,
                                                 bool include_sensors = false) const;

    // ---- Multi-hit queries (A10.1) ------------------------------------------
    // Filtered by object layer (LayerMask; characters are on Layer::character
    // through their inner bodies). At most `max_hits` (capped at
    // max_query_hits) results, one per body or character.
    static constexpr std::size_t max_query_hits = 256;
    enum class QueryShape : std::uint8_t { sphere, box, capsule };
    struct ShapeDesc {
        QueryShape type = QueryShape::sphere;
        float      radius = 0.5F;          // sphere, capsule
        glm::vec3  half_extents{0.5F};     // box
        float      half_height = 0.5F;     // capsule: half the cylinder length (along Y)
    };
    struct QueryHit {
        std::uint32_t body = invalid_body;      // the body (a character's inner body for characters)
        std::uint32_t character = invalid_body; // set when a character was hit
        std::uint32_t owner = no_owner;
        Layer         layer = Layer::static_body;
        bool          sensor = false;
        glm::vec3     point{0.0F};  // world-space contact point
        glm::vec3     normal{0.0F}; // world-space surface normal of the hit body, facing the query
        float         fraction = 0.0F;    // casts: along the cast, 0..1
        float         distance = 0.0F;    // casts: metres travelled before the hit
        float         penetration = 0.0F; // overlaps: how deep the shapes overlap, metres
    };
    // Every hit along a ray, nearest first.
    [[nodiscard]] std::vector<QueryHit> cast_ray_all(const glm::vec3& origin,
                                                     const glm::vec3& direction,
                                                     float max_distance,
                                                     LayerMask layers = solid_layers,
                                                     std::size_t max_hits = 64) const;
    // Bodies a shape at `position` / `rotation` overlaps, deepest first.
    [[nodiscard]] std::vector<QueryHit> overlap(const ShapeDesc& shape, const glm::vec3& position,
                                                const glm::quat& rotation,
                                                LayerMask layers = solid_layers,
                                                std::size_t max_hits = 64) const;
    // Bodies a shape swept from `from` to `to` (fixed rotation) touches,
    // nearest first (fraction 0: already overlapping at `from`).
    [[nodiscard]] std::vector<QueryHit> cast_shape(const ShapeDesc& shape, const glm::vec3& from,
                                                   const glm::vec3& to, const glm::quat& rotation,
                                                   LayerMask layers = solid_layers,
                                                   std::size_t max_hits = 64) const;

private:
    PhysicsWorld() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace engine::physics
