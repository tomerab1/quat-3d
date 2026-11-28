#pragma once

// Scene + system scheduler (Phase 4, Slice 4.2).
//
// A Scene owns the entt::registry and runs a fixed, explicit sequence of systems
// each tick. Systems are stateless free functions (per CLAUDE.md); the Scene
// just sequences them and holds the per-tick outputs they produce (the draw
// list). Camera and glTF-graph integration arrive in 4.3 / 4.4.

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/core/error.hpp"
#include "engine/scene/animation_system.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/navigation.hpp"
#include "engine/terrain/edits.hpp"

namespace engine::core {
class EventRing;
}
namespace engine::nav {
class NavMesh;
}
namespace engine::physics {
class PhysicsWorld;
}
// The draw list's element type is only named here so this header stays free of
// the renderer and the graphics API (P8: it is part of the installed SDK);
// code that reads draw_list() includes engine/renderer/draw_batching.hpp.
namespace engine::renderer {
struct DrawItem;
}

namespace engine::scene {

class Scene {
public:
    // Out of line: the members' special functions need the complete DrawItem.
    Scene();
    ~Scene();
    Scene(Scene&&) noexcept;
    Scene& operator=(Scene&&) noexcept;
    Scene(const Scene&)            = delete;
    Scene& operator=(const Scene&) = delete;

    [[nodiscard]] entt::registry& registry() noexcept { return registry_; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return registry_; }

    // Create an entity carrying a Name and an identity Transform — the minimum
    // every scene node has. Components are added by the caller afterwards.
    entt::entity create_entity(std::string name = {});

    // Re-parent `child` under `parent` (entt::null detaches to a root),
    // maintaining the Parent component on the child and the Children list on both
    // the old and new parent. Refused (returns false, hierarchy unchanged) when
    // either entity is invalid, when child == parent, or when `parent` is a
    // descendant of `child` — any of those would create a cycle that drops the
    // subtree out of transform propagation and makes subtree walks recurse
    // forever (scene files and remote requests are untrusted).
    bool set_parent(entt::entity child, entt::entity parent);

    // Run the fixed system sequence for one tick (`dt` seconds since the last):
    //   1. TransformSystem     — propagate world matrices down the hierarchy.
    //   2. AnimationSystem     — advance Animators, place feet (probing the
    //                            ground with `ground`), write skinning matrices.
    //   3. WeatherSystem       — evolve the weather's wetness and snow cover
    //                            (engine/weather/weather_state.hpp).
    //   4. WorldClockSystem    — advance the scene's WorldClock, derive the
    //                            sky, drive the sun (world_clock.hpp).
    //   5. RenderCollectSystem — gather drawable entities into draw_list().
    void tick(float dt = 0.0F, const FootGroundProbe& ground = {});

    // First entity with an active Camera, or entt::null if none.
    [[nodiscard]] entt::entity active_camera() const;

    // Draw list produced by the most recent tick(). Pointers reference assets
    // owned elsewhere (the AssetManager); valid until those assets are released.
    [[nodiscard]] const std::vector<renderer::DrawItem>& draw_list() const noexcept {
        return draw_list_;
    }

private:
    entt::registry                  registry_;
    std::vector<renderer::DrawItem> draw_list_;
};

// ---------------------------------------------------------------------------
// Systems — stateless, exposed for explicit ordering and direct testing.
// ---------------------------------------------------------------------------

// animation_system (Animator playback, FootIK, skinning): animation_system.hpp.

// Propagate world-space transforms down the Parent/Children hierarchy:
// world = parent.world * local, starting from roots (entities with no Parent, or
// Parent == entt::null).
void transform_system(entt::registry& registry);

// Place `e` at world pose `world` without leaving its parent's space: sets
// Transform.world and rewrites Transform.local = inverse(parent.world) * world,
// so the next transform_system pass reproduces the pose. Roots (no Parent, or
// an invalid one) get local = world. Systems that simulate in world space
// (physics bodies, character controllers, kinematic nav agents) write their
// results back through this. No-op without a Transform.
void set_world_transform(entt::registry& registry, entt::entity e, const glm::mat4& world);

// Gather every entity carrying Transform + MeshRenderer into `out` as DrawItems
// (model = Transform.world, resolved mesh/material pointers). `out` is cleared
// first. Entities with an unloaded/empty mesh are still emitted; the GBuffer
// pass skips draws whose mesh is not renderable.
void render_collect_system(const entt::registry& registry,
                           std::vector<renderer::DrawItem>& out);

// View / projection matrices for the active camera, derived from its world
// Transform and Camera parameters. The projection uses a reverse-friendly [0,1]
// depth range (GLM_FORCE_DEPTH_ZERO_TO_ONE) with the Vulkan clip-space Y flip
// baked in. Identity matrices if there is no active camera.
struct CameraMatrices {
    glm::mat4 view{1.0F};
    glm::mat4 projection{1.0F};
    glm::mat4 view_proj{1.0F};
    glm::vec3 position{0.0F};
};

// Compute the active camera's matrices for the given viewport aspect ratio
// (width / height). Runs TransformSystem's output (reads Transform.world), so
// tick() the scene first.
[[nodiscard]] CameraMatrices camera_system(const entt::registry& registry, float aspect_ratio);

// First entity carrying an active Camera + Transform, or entt::null. The
// editor's fly camera is editor state (not an entity), so "a scene camera
// exists" is what decides whether play mode renders through the scene.
[[nodiscard]] entt::entity find_active_camera(const entt::registry& registry);

// R20: the lens of the view when it renders through the scene: the active
// camera's lens, or `fallback` (the fly camera's) when there is none. Remote
// camera.get / camera.set read it in builds without the editor, so a pose
// edit round-trips the authored lens instead of the fly camera's defaults.
[[nodiscard]] renderer::PhysicalCamera active_camera_lens(const entt::registry& registry,
                                                          const renderer::PhysicalCamera& fallback);

// Bridge ECS <-> physics: create a body for each new RigidBody+Collider+Transform
// entity, push kinematic transforms into the world, step it by `dt`, then write
// dynamic bodies' transforms back into Transform (through the parent's space,
// see set_world_transform).
void physics_system(entt::registry& registry, physics::PhysicsWorld& world, float dt);

// Joint components -> Jolt constraints (A10.1): removes constraints whose
// Joint is gone or was edited, and builds one for each Joint whose bodies
// both exist (its RigidBody's, and the target's or the world), at least one
// dynamic. Anchors and axes are converted with the entities' current world
// transforms. physics_system runs it right after creating bodies.
void joint_system(entt::registry& registry, physics::PhysicsWorld& world);

// Pushes the contact / trigger events of the world's last update() into
// `ring` (A10.1 events.poll), body owners as entity ids, and records the
// contacts the world had to drop as a gap. Call once after each
// physics_system / PhysicsWorld::update.
void publish_physics_events(const physics::PhysicsWorld& world, core::EventRing& ring);
// The body of `e`'s RigidBody, created now (as physics_system's first step
// would) when it has none yet — so gameplay code can push a body it spawned
// this frame. invalid_body without RigidBody + Collider + Transform.
std::uint32_t ensure_rigid_body(entt::registry& registry, physics::PhysicsWorld& world,
                                entt::entity e);

// Create a static Jolt height-field body for the first Terrain entity in the
// registry, matching the rendered surface: the body sits at the tile's minimum
// XZ corner (entity translation - tile_size/2, heights offset by translation.y)
// and the grid is edge-padded by one duplicated row/column so the sample count
// satisfies Jolt's block alignment while every interior cell aligns exactly
// with the rendered texels. Returns the body id (PhysicsWorld::invalid_body
// when there is no Terrain entity or the map is invalid).
[[nodiscard]] std::uint32_t add_terrain_body(entt::registry& registry,
                                             physics::PhysicsWorld& world,
                                             const terrain::Heightmap& map);

// Play-mode body promotion (Unity convention): give every Collider without a
// RigidBody a body — static for scenery, kinematic for NavAgent entities
// (they move through the ECS) — and none for CharacterController entities
// (the controller is their physics presence). Returns the bodies added.
std::size_t promote_collider_only_bodies(entt::registry& registry);

// Gather world-space triangles of the static, walkable scene for navmesh
// baking (13.1): the terrain heightmap (decimated to keep triangle counts
// sane) plus every static box collider — not those on NavAgent or
// CharacterController entities, which move. `map`/`anchor` may describe a
// terrain tile (map null = no terrain).
void collect_nav_geometry(entt::registry& registry, const terrain::Heightmap* map,
                          const glm::vec3& anchor, std::vector<glm::vec3>& verts,
                          std::vector<std::uint32_t>& indices, bool include_colliders = true);

// Tile-level variant for streamed worlds: a static height-field body whose
// minimum XZ corner sits at `origin` (12.4 creates/destroys one per tile).
[[nodiscard]] std::uint32_t add_height_field_body(physics::PhysicsWorld& world,
                                                  const terrain::Heightmap& map,
                                                  const glm::vec3& origin);

// Headroom the height field reserves above and below the tile's heights at
// creation, so terrain edits (G19.1) update it in place.
inline constexpr float height_field_edit_headroom_m = 128.0F;

// Rewrites the texels `rect` of a tile's height-field body (from
// add_height_field_body / add_terrain_body) from `map` in place, block
// aligned, waking what rests there (terrain edits, G19.1). False when the
// body cannot take the change (an edit beyond the reserved headroom): the
// caller then replaces the body.
[[nodiscard]] bool update_height_field_body(physics::PhysicsWorld& world, std::uint32_t body,
                                            const terrain::Heightmap& map,
                                            const terrain::TexelRect& rect);

// Advance every CharacterController: integrate gravity, apply `move` * `speed`
// horizontally, move + resolve collisions via the character controller, and write
// the result back into Transform: only the world translation changes — the
// entity's rotation and scale are kept and parented entities stay in their
// parent's space. NetDriven characters are skipped (the network
// layer steps them per input with step_character).
void character_system(entt::registry& registry, physics::PhysicsWorld& world, float dt);

// One character_system step for one entity (creating its CharacterVirtual on
// first use). No-op unless `e` has a CharacterController and a Transform.
void step_character(entt::registry& registry, physics::PhysicsWorld& world, entt::entity e,
                    float dt);

// nav_agent_system and the other navigation entry points: navigation.hpp.

// Builds a small hierarchy, ticks the scene, and verifies world-matrix
// propagation and draw-list collection. No device needed.
[[nodiscard]] std::expected<void, core::Error> run_scene_self_test();

} // namespace engine::scene
