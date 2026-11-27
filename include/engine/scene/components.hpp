#pragma once

// Core ECS components (Phase 4, Slice 4.1).
//
// Plain-old-data components driven by systems in `scene.cpp`. Per CLAUDE.md:
// components carry no logic — that lives in systems. Only the components needed
// through Phase 4 are defined here (Transform, MeshRenderer, Camera,
// DirectionalLight, Name, Parent, Children); skinning/physics/light components
// arrive with their respective phases.

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/audio/audio_types.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/terrain/generator.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/core/error.hpp"

namespace engine::scene {

// Local transform plus its resolved world-space matrix. `local` is authored
// (or composed from glTF TRS at load); `world` is produced each tick by
// TransformSystem as parent.world * local (identity parent for roots).
struct Transform {
    glm::mat4 local{1.0F};
    glm::mat4 world{1.0F};
};

// A renderable: which mesh to draw with which material. Null/unloaded handles
// resolve to the manager's per-type defaults, so a partially-built entity still
// renders something rather than crashing.
struct MeshRenderer {
    asset::AssetHandle<asset::MeshAsset>     mesh;
    asset::AssetHandle<asset::MaterialAsset> material;
};

// Provenance of a MeshRenderer's assets, for scene serialization (editor save/
// load): the AssetManager cache keys, plus the glTF file that produces them.
// An empty gltf_path means engine code creates the keyed assets at startup
// (e.g. the showcase primitives), so a loaded scene resolves them by cache hit.
// `gltf_node` is the glTF node that instanced the mesh: one mesh can be
// instanced by several nodes with different skins (and armature offsets), so
// a loaded scene re-attaches skins by (mesh_key, node), not by mesh alone.
struct MeshSource {
    std::string mesh_key;
    std::string material_key;   // empty -> default material
    std::string gltf_path;      // empty -> code-created assets
    int         gltf_node = -1; // -1: not instanced from a glTF node (or unknown)
};

// A procedural shape with a flat PBR material (Phase 15) — the data-driven way
// to make geometry without a glTF file (remote/AI world building, blockouts).
// Shapes are unit-sized and centred on the entity origin (cube 1 m, sphere /
// cylinder / cone / capsule 1 m diameter, cylinder and cone 1 m tall, capsule
// 2 m tall, plane 1x1 m facing +Y); size them with the Transform scale.
// resolve_primitives() (primitives.hpp) builds the MeshRenderer from this —
// meshes and materials are shared through the AssetManager by content — and
// rebuilds it whenever the fields change. `resolved` is system-managed.
enum class PrimitiveShape : std::uint8_t { cube, sphere, plane, cylinder, cone, capsule };
struct Primitive {
    PrimitiveShape shape = PrimitiveShape::cube;
    glm::vec4      color{0.8F, 0.8F, 0.8F, 1.0F}; // linear base colour; alpha < 1 blends
    float          metallic = 0.0F;
    float          roughness = 0.5F;
    glm::vec3      emissive{0.0F};                // linear emitted radiance (HDR)
    std::uint64_t  resolved = 0;                  // hash the MeshRenderer was built from
};

// One placement of an InstancedMesh, relative to the entity (R9.2): the
// instance's model matrix is entity.world * T(position) * R(rotation) *
// S(scale).
struct InstanceTransform {
    glm::vec3 position{0.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F}; // (w, x, y, z): identity
    glm::vec3 scale{1.0F};
};

// Many placements of one model on a single entity (R9.2) — forests, rocks,
// props — drawn as one instanced draw per (mesh, material) instead of one
// entity per copy. The model is a glTF file (`gltf_path`, relative to the
// project root; the whole scene, or only the subtree of the node named
// `gltf_node`) or, when `gltf_path` is empty, `primitive`'s shape and flat
// material (its `resolved` field is unused here). `instances` are authored
// placements, or the output of a Scatter on the same entity (which rewrites
// them whenever it regenerates). resolve_instanced_meshes()
// (instanced_mesh.hpp) builds the InstancedMeshParts the renderer draws.
struct InstancedMesh {
    std::string                    gltf_path;
    std::string                    gltf_node;
    Primitive                      primitive{};
    std::vector<InstanceTransform> instances;
    bool                           cast_shadows = true;
};

// One (mesh, material) of an InstancedMesh's model; `local` places it in the
// instance's frame (the glTF node hierarchy below the chosen root).
struct InstancedMeshPart {
    asset::AssetHandle<asset::MeshAsset>     mesh;
    asset::AssetHandle<asset::MaterialAsset> material;
    glm::mat4                                local{1.0F};
};

// System-managed: the resolved model of an InstancedMesh (like the
// MeshRenderer a Primitive resolves into). `bounds` is the bounding sphere of
// all parts in the instance's frame (xyz centre, w radius) for culling;
// `resolved` is the source hash the parts were built from.
struct InstancedMeshParts {
    std::vector<InstancedMeshPart> parts;
    glm::vec4                      bounds{0.0F};
    std::uint64_t                  resolved = 0;
};

// Seeded procedural placement on the ground (R9.2): generates the entity's
// InstancedMesh::instances (adding a default InstancedMesh if it has none).
// The area is centred on the entity's world position and axis-aligned in
// world XZ: a `size` rectangle (metres) or a `radius` circle. Candidates are
// Poisson-disc samples (blue noise; Bridson) or a jittered grid at `density`
// instances per m², never closer than `min_spacing`; each one samples the
// terrain (streamed tiles too; flat ground at the entity's height without
// terrain) and is kept only when the ground slope (degrees) and height
// (metres) fall within the limits. Each placement gets a uniform scale in
// `scale_range`, an optional random yaw, tilts `align_to_normal` (0 upright
// .. 1 along the ground normal) and is lifted `base_offset` * scale along
// its up axis (0.5 seats a unit primitive on the ground). Generation is
// deterministic per seed, and a candidate's fate depends only on its own
// ground sample, so terrain streaming in or out never reshuffles the rest.
// scatter_system (scatter.hpp) regenerates whenever the parameters, the
// entity's transform or the terrain under the area change; `generated` (key
// of the last generation) and `generation` (count) are system-managed.
// Removing the Scatter keeps the current instances (baked).
enum class ScatterArea : std::uint8_t { rect, circle };
enum class ScatterPattern : std::uint8_t { poisson, jittered_grid };
struct Scatter {
    std::uint32_t  seed = 1;
    ScatterArea    area = ScatterArea::rect;
    glm::vec2      size{40.0F, 40.0F}; // rect: extent along world X and Z, metres
    float          radius = 20.0F;     // circle
    float          density = 0.05F;    // instances per m^2
    float          min_spacing = 2.0F; // metres
    ScatterPattern pattern = ScatterPattern::poisson;
    glm::vec2      scale_range{0.8F, 1.2F};
    bool           random_yaw = true;
    float          align_to_normal = 0.0F;
    float          base_offset = 0.0F;
    glm::vec2      slope_deg{0.0F, 35.0F};       // keep ground slopes in [x, y]
    glm::vec2      height_m{-10000.0F, 10000.0F}; // keep ground heights in [x, y]
    std::uint32_t  max_instances = 20000;
    // System-managed.
    std::uint64_t  generated = 0;
    std::uint32_t  generation = 0;
};

// Marks an entity (alongside its MeshRenderer) as skinned: its mesh deforms with
// `skeleton`. `joint_matrices` holds the per-joint skinning matrices the
// animation system writes each frame (joint_world * inverse_bind); the GPU
// skinning pass (5.4) consumes them. Empty until an AnimationSystem fills it.
struct SkinnedMesh {
    asset::AssetHandle<animation::SkeletonAsset> skeleton;
    std::vector<glm::mat4>                       joint_matrices;
    // Load-time offset from this entity's frame to the skeleton's root ancestor
    // (the "armature" node above the joints): inverse(mesh node global) *
    // armature global, both in glTF scene space. The animation system folds the
    // entity's current Transform.world on top each tick, so the skinned mesh
    // follows the entity when it is moved. Identity when armature == mesh node.
    glm::mat4                                    root_transform{1.0F};
};

// Plays an animation clip on a skinned entity. The AnimationSystem advances
// `time` by dt*speed each tick (wrapping when `looping`), samples the clip, and
// writes the result into the entity's SkinnedMesh::joint_matrices.
struct Animator {
    asset::AssetHandle<animation::AnimClipAsset> clip;
    float time    = 0.0F;
    float speed   = 1.0F;
    bool  looping = true;
};

// Perspective camera parameters. The view/projection matrices are derived each
// frame by CameraSystem from this plus the entity's world Transform (4.3).
// Aspect ratio is supplied by the renderer from the swapchain extent.
struct Camera {
    float fov_y  = glm::radians(60.0F); // vertical field of view, radians
    float near_z = 0.1F;
    float far_z  = 1000.0F;
    bool  is_active = true;
};

// Collision shape for a physics body. Primitive shapes only; `half_extents`
// means box half-extents, sphere radius in x, or capsule (radius in x,
// half-height in y). `offset` reserved for shape-local offset (unused for now).
enum class ColliderShape : std::uint8_t { box, sphere, capsule };
struct Collider {
    ColliderShape shape = ColliderShape::box;
    glm::vec3     half_extents{0.5F};
    glm::vec3     offset{0.0F};
    bool          is_sensor = false; // a trigger volume (overlap events, no response)
};

// A physics body. `body` is the handle assigned by the PhysicsSystem when it
// creates the Jolt body from this entity's Collider + Transform (invalid until
// then). Static bodies never move; kinematic are driven ECS->physics; dynamic
// are driven physics->ECS.
enum class BodyMotion : std::uint8_t { static_body, kinematic, dynamic };
struct RigidBody {
    BodyMotion    motion = BodyMotion::dynamic;
    float         mass = 1.0F;
    std::uint32_t body = 0xFFFFFFFFU; // PhysicsWorld body handle
};

// A capsule character controller (predictive, no rigid body). `move` is the
// desired horizontal direction (xz, magnitude <= 1) the controller reads each
// tick; `velocity`/`on_ground`/`character` are system-managed.
struct CharacterController {
    float         radius = 0.3F;
    float         half_height = 0.6F; // capsule cylinder half-height
    float         speed = 4.0F;
    glm::vec3     move{0.0F};
    glm::vec3     velocity{0.0F};
    bool          on_ground = false;
    std::uint32_t character = 0xFFFFFFFFU;
};

// Autonomous navigation: the agent repaths on the baked navmesh whenever
// `target` changes and follows the waypoints at `speed`. With a
// CharacterController on the entity it steers the controller (collisions,
// gravity); otherwise it moves the Transform kinematically, snapped to the
// ground + `ground_offset` (set it to the entity's half-height when the pivot
// sits at the centre). A walk ends either `arrived` (reached the target, or
// the closest on-mesh point to it) or `failed` (the target is unreachable,
// plans kept failing, or the agent made no progress) — both clear `active`.
// `paused` (the inspector's Stop) holds the agent: patrol routes do not
// restart it until it is resumed. `commanded` marks a one-off order (Ctrl+
// click, Go): a PatrolRoute waits for it to end, then resumes its route.
// Everything below `paused` is system-managed (not serialized); start walks
// through the helpers in navigation.hpp, which reset it.
struct NavAgent {
    glm::vec3 target{0.0F};
    float     speed = 3.0F;
    float     ground_offset = 0.0F; // entity origin height above the ground
    bool      active = false;
    bool      paused = false;
    bool      arrived = false;
    bool      commanded = false;       // the target is a one-off order, not a route point
    bool      failed = false;          // gave up on the current target
    std::uint8_t           plan_failures = 0; // failed attempts since the last progress
    bool                   path_partial = false; // path ends short of the target
    std::vector<glm::vec3> path;
    std::size_t            waypoint = 0;
    glm::vec3              planned_target{1e30F, 1e30F, 1e30F}; // path's target
    std::uint64_t          planned_generation = 0; // NavMesh::generation() of the path
    float                  retry_s = 0.0F; // cooldown after a failed plan
    float                  progress_best = 1e30F; // closest approach to the current waypoint
    float                  progress_timer = 0.0F; // time since progress_best improved
};

// Per-entity, per-node behaviour-tree memory (system-managed).
struct BtNodeState {
    float         scratch = 0.0F; // leaf memory (wait timer, move_to phase)
    std::uint32_t child = 0;      // composites: the child that was running
    std::uint32_t ticked = 0;     // tick stamp of the last tick that reached it
    bool          running = false; // returned running on that tick
};

// What a compiled node returned on its tree's last tick (idle: not reached).
// Unlike BtNodeState it survives the resets that end a tick, so tools can
// show which branch failed (the remote bt.status method).
enum class BtNodeResult : std::uint8_t { idle, success, failure, running };

// A data-driven behaviour tree (13.3): `source` is the tree JSON (composed of
// sequence/selector/reactive_*/inverter nodes over named C++ leaves — see
// behavior.hpp). The system compiles it lazily; a parse error logs once and
// disables the tree. compiled/node_state/last_result/tick_count/parse_failed
// are system-managed.
struct BehaviorTree {
    std::string source;
    bool        enabled = true;
    std::shared_ptr<const class BehaviorTreeAsset> compiled;
    std::vector<BtNodeState>  node_state;  // one per compiled node
    std::vector<BtNodeResult> last_result; // one per compiled node
    std::uint32_t            tick_count = 0;
    bool parse_failed = false;
};

// Single directional light (sun). `direction` points from the light toward the
// scene (i.e. the direction photons travel) in world space.
struct DirectionalLight {
    glm::vec3 direction{0.0F, -1.0F, 0.0F};
    glm::vec3 color{1.0F};
    float     intensity = 1.0F;
};

// Point light at the entity's world position; falls off to nothing at `radius`.
struct PointLight {
    glm::vec3 color{1.0F};
    float     radius = 10.0F;
    float     intensity = 1.0F;
};

// Procedural terrain tile anchored at the entity's world position (the tile's
// minimum XZ corner sits at translation - tile_size/2 on each axis, heights
// offset by translation.y). The frame loop owns generation: it kicks an async
// rebuild whenever `regenerate` is set (the inspector's button / fresh
// component) and uploads the result to the terrain pass.
struct Terrain {
    // New terrains erode with the grid model, which the GPU can synthesise
    // (TerrainParams itself defaults to the CPU-only droplet model).
    terrain::TerrainParams params = [] {
        terrain::TerrainParams p;
        p.erosion_model = terrain::ErosionModel::grid;
        return p;
    }();
    float snowline_m = 110.0F; // world height where snow takes over
    bool  regenerate = true;   // consumed by the frame loop
    // Streaming (12.4): keep a (2*radius+1)² window of seamless tiles loaded
    // around the camera instead of one fixed tile.
    bool streaming = false;
    int  stream_radius = 1;
    // R10.2: synthesise tiles on the GPU (milliseconds) when a device can —
    // noise and grid erosion; droplet erosion and oversized tiles fall back
    // to the CPU generator. Physics/navmesh use the read-back heights.
    bool gpu_generate = true;
};

// An authored walk route (editor: Shift+click the ground to append points).
// The nav system feeds the points to the entity's idle NavAgent in order;
// `loop` wraps back to the first point, otherwise the agent stops at the
// last (`finished`; appending points continues the route). Commanded moves
// interrupt the route, which resumes at `next` afterwards; unreachable
// points are skipped. `next`/`finished` are system-managed.
struct PatrolRoute {
    std::vector<glm::vec3> points;
    bool        loop = true;
    std::size_t next = 0;
    bool        finished = false;
};

// Marks an entity for network replication (14.2): the server broadcasts its
// Transform to clients, matched by `net_id` (stable across both ends — both
// load the same scene). 0 is unassigned.
struct NetReplicated {
    std::uint32_t net_id = 0;
};

// A sound emitter (G1), driven by audio_system (audio_system.hpp). `clip` is
// a wav / flac / mp3 path, relative to the project root unless absolute;
// music and ambience clips stream from disk, sfx / ui clips decode into
// memory. Spatial sources sit at the entity's world position and fade with
// the inverse-distance model: full volume within min_distance, rolloff
// beyond it, clamped at max_distance. `doppler` 0 (the default) disables
// the doppler shift. In play mode `play_on_start` starts the source once.
// Scripts / tools steer playback through `command`, which the system
// consumes; sound/state/started/resolved/last_position are system-managed.
enum class AudioCommand : std::uint8_t { none, play, stop, pause, resume };
struct AudioSource {
    std::string     clip;
    audio::AudioBus bus = audio::AudioBus::sfx;
    float           volume = 1.0F; // linear gain
    float           pitch = 1.0F;  // playback-rate multiplier
    bool            loop = false;
    bool            play_on_start = true;
    bool            spatial = true;
    float           min_distance = 1.0F;
    float           max_distance = 50.0F;
    float           rolloff = 1.0F;
    float           doppler = 0.0F;
    AudioCommand    command = AudioCommand::none;
    // System-managed.
    audio::Playback    state = audio::Playback::stopped;
    bool               started = false; // play_on_start consumed this play session
    audio::SoundHandle sound = audio::invalid_sound;
    std::uint64_t      resolved = 0;    // hash of (clip, bus, spatial) the sound was made from
    glm::vec3          last_position{0.0F};
};

// Marks the entity whose world pose the audio listener follows (the "ears").
// The first active one wins; with none, the listener follows the camera the
// frame renders from.
struct AudioListener {
    bool active = true;
};

// Tag: this CharacterController is stepped by the network layer, once per
// player input (host: a remote player's inputs; client: prediction and
// reconciliation), so character_system leaves it alone. Runtime-only.
struct NetDriven {};

// Human-readable label. Editor-only metadata, not on any hot path.
struct Name {
    std::string value;
};

// Scene-graph parent link. `entity` is entt::null for a root. Children are kept
// in sync via the Children component on the parent.
struct Parent {
    entt::entity entity = entt::null;
};

// Scene-graph child list. Maintained alongside Parent so the hierarchy can be
// walked in either direction without a full registry scan.
struct Children {
    std::vector<entt::entity> entities;
};

// Exercises entt integration: spawns entities, attaches the core components,
// and verifies view/group queries observe them. Proves the ECS world links and
// the component types are registry-storable. No device needed.
[[nodiscard]] std::expected<void, core::Error> run_ecs_self_test();

} // namespace engine::scene
