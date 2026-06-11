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
struct MeshSource {
    std::string mesh_key;
    std::string material_key; // empty -> default material
    std::string gltf_path;    // empty -> code-created assets
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
// gravity); otherwise it moves the Transform kinematically. `arrived` flips
// when the last waypoint is reached. path/waypoint are system-managed.
struct NavAgent {
    glm::vec3 target{0.0F};
    float     speed = 3.0F;
    bool      active = false;
    bool      arrived = false;
    std::vector<glm::vec3> path;
    std::size_t            waypoint = 0;
    glm::vec3              planned_target{1e30F, 1e30F, 1e30F}; // path's target
};

// A data-driven behaviour tree (13.3): `source` is the tree JSON (composed of
// sequence/selector/inverter nodes over named C++ leaves — see behavior.hpp).
// The system compiles it lazily; a parse error logs once and disables the
// tree. compiled/node_state/parse_failed are system-managed.
struct BehaviorTree {
    std::string source;
    bool        enabled = true;
    std::shared_ptr<const class BehaviorTreeAsset> compiled;
    std::vector<float> node_state; // per-node scratch (e.g. wait timers)
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
    terrain::TerrainParams params{};
    float snowline_m = 110.0F; // world height where snow takes over
    bool  regenerate = true;   // consumed by the frame loop
    // Streaming (12.4): keep a (2*radius+1)² window of seamless tiles loaded
    // around the camera instead of one fixed tile.
    bool streaming = false;
    int  stream_radius = 1;
};

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
