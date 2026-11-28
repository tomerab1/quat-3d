#pragma once

// GPU particles (ROADMAP R10.3): emitter settings, the CPU side of emission
// (spawn schedule, pool sub-allocation, the per-emitter GPU record) and CPU
// references of the per-particle spawn and integration that
// particle_sim.slang implements — tests compare the two. The passes are in
// particle_pass.hpp.
//
// Every random property of a particle derives from a hash of (emitter seed,
// spawn index): the n-th particle an emitter requests is the same particle on
// every run, whatever pool slot it lands in, so a fixed seed and the same
// sequence of simulation steps reproduce the same set of particles.
//
// Integration is semi-implicit Euler with implicit linear drag, per step of
// dt seconds:
//   v' = (v + g dt) / (1 + drag dt);   p' = p + v' dt;   rot' = rot + spin dt;
//   age' = age + dt   (dead once age' >= lifetime).
// The reciprocal 1 / (1 + drag dt) is computed once on the CPU and handed to
// the GPU (no divisions in the shader), so both sides do the same arithmetic.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace engine::renderer {

enum class ParticleShape : std::uint8_t { point, sphere, box, cone };
// additive: light-emitting (sparks, fire) — blended ONE, ONE into the HDR.
// alpha:    weighted blended order-independent transparency (smoke, dust).
enum class ParticleBlend : std::uint8_t { additive, alpha };
enum class ParticleLighting : std::uint8_t { unlit, lit };
// world: particles keep their world position when the emitter moves.
// local: particles live in the emitter's space and move with it.
enum class ParticleSpace : std::uint8_t { world, local };

inline constexpr std::uint32_t particle_curve_keys = 4;
inline constexpr std::uint32_t particle_max_per_emitter = 65536;
inline constexpr std::uint32_t particle_max_emitters = 256;
inline constexpr std::uint32_t particle_default_pool = 1U << 18;
inline constexpr float         particle_max_step = 0.1F; // longer frames simulate 0.1 s

// One key of a curve over the normalised age (0 = born, 1 = dies).
struct ParticleColorKey {
    float     t = 0.0F;
    glm::vec4 color{1.0F}; // linear RGB + alpha
};
struct ParticleSizeKey {
    float t = 0.0F;
    float size = 1.0F; // multiplier of the particle's start size
};

// Every field is in metres / seconds / degrees; colours are linear.
struct ParticleEmitterSettings {
    bool          enabled = true;        // false: no new particles (live ones finish)
    std::uint32_t seed = 1;
    std::uint32_t max_particles = 1024;  // this emitter's share of the global pool
    float         rate = 50.0F;          // particles per second
    std::uint32_t burst_count = 0;       // particles per burst (0 = none)
    float         burst_interval = 0.0F; // seconds between bursts (0 = one burst at start)
    glm::vec2     lifetime{1.0F, 2.0F};  // [min, max] seconds

    ParticleShape shape = ParticleShape::point;
    float         radius = 0.5F;              // sphere / cone base radius
    glm::vec3     box_half_extents{0.5F};
    float         cone_angle_deg = 25.0F;     // cone half angle
    glm::vec3     direction{0.0F, 1.0F, 0.0F}; // emitter-local launch direction (cone axis)
    float         spread_deg = 15.0F;          // random deviation (point / box)
    glm::vec2     speed{1.0F, 2.0F};           // [min, max] m/s

    glm::vec3 gravity{0.0F, -9.81F, 0.0F}; // world space, m/s^2
    float     drag = 0.0F;                 // 1/s

    glm::vec2 size{0.2F, 0.4F};             // start width [min, max] m
    glm::vec2 rotation_deg{0.0F, 360.0F};   // start rotation [min, max]
    glm::vec2 spin_deg{-45.0F, 45.0F};      // angular velocity [min, max] deg/s
    std::array<ParticleSizeKey, particle_curve_keys>  size_curve{};
    std::uint32_t                                     size_keys = 1;
    std::array<ParticleColorKey, particle_curve_keys> color_curve{
        {{0.0F, glm::vec4(1.0F)}, {1.0F, glm::vec4(1.0F, 1.0F, 1.0F, 0.0F)}}};
    std::uint32_t                                     color_keys = 2;
    float emissive = 1.0F; // HDR brightness multiplier (unlit / additive)

    // Flipbook atlas: columns x rows cells, `frames` used row by row; played
    // at `fps`, or once over the lifetime when fps is 0. Empty path: a
    // built-in soft disc. Paths are relative to the project root, under assets/.
    std::string texture;
    glm::uvec2  flipbook_grid{1, 1};
    std::uint32_t flipbook_frames = 1;
    float       flipbook_fps = 0.0F;

    ParticleBlend    blend = ParticleBlend::additive;
    ParticleLighting lighting = ParticleLighting::unlit;
    ParticleSpace    space = ParticleSpace::world;
    float            soft_distance = 0.3F; // depth fade near geometry (0 = hard edges)
    bool             sort = false;         // back-to-front sort (stable draw order)

    // Depth-buffer collision against the GBuffer depth + normals (on-screen
    // geometry only).
    bool  collide = false;
    float bounce = 0.3F;   // restitution of the normal velocity
    float friction = 0.2F; // fraction of tangential velocity lost per contact
};

// Clamps every field into its valid range (codec ranges), orders [min, max]
// pairs, sorts curve keys by time and normalises the direction.
[[nodiscard]] ParticleEmitterSettings sanitize(const ParticleEmitterSettings& settings);

// ---- Emission schedule ------------------------------------------------------

// Per-emitter emission state (renderer-side, not ECS data).
struct ParticleSpawnState {
    double        time_s = 0.0;     // simulated time since the emitter started
    double        carry = 0.0;      // fractional particles owed by `rate`
    double        next_burst_s = 0.0;
    bool          bursts_done = false;
    std::uint32_t spawned = 0;      // particles requested so far (RNG stream index, wraps)
};

// Particles to request this step of `dt` seconds (0 when dt <= 0): `rate`
// accumulates fractional particles, bursts fire when their time is reached.
// At most `max_particles` per step (the excess is dropped, not deferred).
// Advances `state.spawned` by the returned count.
[[nodiscard]] std::uint32_t advance_spawn(ParticleSpawnState& state,
                                          const ParticleEmitterSettings& settings, float dt);

// ---- Pool sub-allocation ----------------------------------------------------

// First-fit allocator of contiguous particle ranges in the global pool;
// freed ranges merge with their neighbours.
class ParticlePoolAllocator {
public:
    explicit ParticlePoolAllocator(std::uint32_t capacity = particle_default_pool);

    // Base index of a free range of `count` particles, or UINT32_MAX.
    [[nodiscard]] std::uint32_t allocate(std::uint32_t count);
    void free(std::uint32_t base, std::uint32_t count);

    [[nodiscard]] std::uint32_t capacity() const { return capacity_; }
    [[nodiscard]] std::uint32_t free_total() const;

private:
    struct Range {
        std::uint32_t base = 0;
        std::uint32_t count = 0;
    };
    std::uint32_t      capacity_ = 0;
    std::vector<Range> free_; // sorted by base, never adjacent
};

// ---- GPU records (mirrored by lib/particle_common.slang) --------------------

// One particle (std430, 48 bytes). Position / velocity are in simulation
// space (world, or the emitter's local space).
struct ParticleGpu {
    glm::vec3     position{0.0F};
    float         age = 0.0F;
    glm::vec3     velocity{0.0F};
    float         lifetime = 1.0F;
    float         rotation = 0.0F; // radians
    float         spin = 0.0F;     // radians / s
    float         size = 1.0F;     // start width, m
    std::uint32_t random = 0;      // per-particle random bits (flipbook phase, tint)
};
static_assert(sizeof(ParticleGpu) == 48, "must match particle_common.slang Particle");

// EmitterGpu::config.y bits.
inline constexpr std::uint32_t particle_flag_local = 1U << 0;
inline constexpr std::uint32_t particle_flag_collide = 1U << 1;
inline constexpr std::uint32_t particle_flag_lit = 1U << 2;
inline constexpr std::uint32_t particle_flag_additive = 1U << 3;
inline constexpr std::uint32_t particle_flag_reset = 1U << 4;
inline constexpr std::uint32_t particle_flag_sort = 1U << 5;

// One emitter for one frame (std430, 416 bytes).
struct EmitterGpu {
    glm::vec4  world[4]{};     // emitter -> world (columns)
    glm::vec4  inv_world[4]{}; // world -> emitter (columns)
    glm::uvec4 range{0};       // pool base, capacity, particles requested, first spawn index
    glm::uvec4 config{0};      // shape, flags, seed, curve key counts (color | size << 8)
    glm::vec4  shape{0.0F};    // box half extents, radius
    glm::vec4  direction{0.0F}; // unit launch direction (emitter space), 1 - cos(spread or cone)
    glm::vec4  speed_life{0.0F}; // speed min, max; lifetime min, max
    glm::vec4  forces{0.0F};     // gravity (simulation space), 1 / (1 + drag dt)
    glm::vec4  size_rot{0.0F};   // size min, max; rotation min, max (radians)
    glm::vec4  spin_hit{0.0F};   // spin min, max (rad/s); bounce; 1 - friction
    glm::vec4  flipbook{0.0F};   // columns, rows, frames, fps
    glm::vec4  render{0.0F};     // 1 / soft distance (0 = hard), emissive, collision thickness
    glm::uvec4 ids{0};           // sort key base, key count (power of two; 0 = unsorted),
                                 // emitter slot (counters + indirect arguments)
    glm::vec4  color_value[particle_curve_keys]{};
    glm::vec4  color_time{0.0F};
    glm::vec4  size_value{0.0F};
    glm::vec4  size_time{0.0F};
};
static_assert(sizeof(EmitterGpu) == 416, "must match particle_common.slang Emitter");

// Per-frame inputs of one emitter beyond its settings.
struct EmitterFrameInfo {
    glm::mat4     world{1.0F};
    std::uint32_t slot = 0;        // stable id < particle_max_emitters: its counters and
                                   // indirect arguments (keep it while the emitter lives)
    std::uint32_t pool_base = 0;
    std::uint32_t spawn_count = 0; // requested this frame (advance_spawn)
    std::uint32_t spawn_index = 0; // ParticleSpawnState::spawned before this frame's request
    float         dt = 0.0F;
    bool          reset = false;   // (re)initialise the emitter's pool range first
    std::uint32_t sort_base = 0;   // where its sort keys live (sorted emitters)
};

// Depth band behind the visible surface within which a particle counts as
// touching it (thicker would catch particles passing behind objects).
inline constexpr float particle_collision_thickness = 0.5F;

[[nodiscard]] EmitterGpu make_emitter_gpu(const ParticleEmitterSettings& settings,
                                          const EmitterFrameInfo& info);

// Sort keys an emitter needs (its capacity rounded up to a power of two, at
// least the bitonic block of 512); 0 when it is not sorted.
[[nodiscard]] std::uint32_t particle_sort_size(const ParticleEmitterSettings& settings);

// ---- CPU references -----------------------------------------------------------

// The shader's integer hash (PCG output permutation) and [0, 1) float from 24 bits.
[[nodiscard]] std::uint32_t particle_hash(std::uint32_t v);
[[nodiscard]] float         particle_random01(std::uint32_t seed, std::uint32_t index,
                                              std::uint32_t stream);

// The particle `spawn_index` of `emitter` is born as (simulation space).
[[nodiscard]] ParticleGpu particle_spawn(const EmitterGpu& emitter, std::uint32_t spawn_index);

// One integration step (no collision). Returns false when the particle dies.
[[nodiscard]] bool particle_integrate(ParticleGpu& particle, const EmitterGpu& emitter, float dt);

// Piecewise-linear curve values at normalised age `t` (clamped at the ends).
[[nodiscard]] glm::vec4 particle_color_at(const EmitterGpu& emitter, float t);
[[nodiscard]] float     particle_size_at(const EmitterGpu& emitter, float t);

// McGuire & Bavoil's depth weight (JCGT 2013, eq. 9) for an alpha `a` at
// view depth `z` metres.
[[nodiscard]] float wboit_weight(float alpha, float view_depth);

} // namespace engine::renderer
