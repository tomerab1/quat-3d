#pragma once

// Scene + system scheduler (Phase 4, Slice 4.2).
//
// A Scene owns the entt::registry and runs a fixed, explicit sequence of systems
// each tick. Systems are stateless free functions (per CLAUDE.md); the Scene
// just sequences them and holds the per-tick outputs they produce (the draw
// list). Camera and glTF-graph integration arrive in 4.3 / 4.4.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/core/error.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/scene/components.hpp"

namespace engine::nav {
class NavMesh;
}
namespace engine::physics {
class PhysicsWorld;
}

namespace engine::scene {

class Scene {
public:
    Scene() = default;

    [[nodiscard]] entt::registry& registry() noexcept { return registry_; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return registry_; }

    // Create an entity carrying a Name and an identity Transform — the minimum
    // every scene node has. Components are added by the caller afterwards.
    entt::entity create_entity(std::string name = {});

    // Re-parent `child` under `parent` (entt::null detaches to a root),
    // maintaining the Parent component on the child and the Children list on both
    // the old and new parent.
    void set_parent(entt::entity child, entt::entity parent);

    // Run the fixed system sequence for one tick (`dt` seconds since the last):
    //   1. AnimationSystem     — advance Animators, write skinning matrices.
    //   2. TransformSystem     — propagate world matrices down the hierarchy.
    //   3. RenderCollectSystem — gather drawable entities into draw_list().
    void tick(float dt = 0.0F);

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

// Advance every Animator by `dt` (scaled by its speed, wrapped when looping),
// sample its clip against the entity's SkinnedMesh skeleton, and write the
// per-joint skinning matrices into SkinnedMesh::joint_matrices. Entities with a
// SkinnedMesh but no/!loaded Animator clip get the bind pose.
void animation_system(entt::registry& registry, float dt);

// Propagate world-space transforms down the Parent/Children hierarchy:
// world = parent.world * local, starting from roots (entities with no Parent, or
// Parent == entt::null).
void transform_system(entt::registry& registry);

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

// Bridge ECS <-> physics: create a body for each new RigidBody+Collider+Transform
// entity, push kinematic transforms into the world, step it by `dt`, then write
// dynamic bodies' transforms back into Transform (entities are treated as roots).
void physics_system(entt::registry& registry, physics::PhysicsWorld& world, float dt);

// Drops a dynamic sphere entity onto a static box-floor entity through the ECS
// physics system and verifies its Transform fell and came to rest on the floor.
[[nodiscard]] std::expected<void, core::Error> run_physics_body_self_test();

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

// Gather world-space triangles of the static, walkable scene for navmesh
// baking (13.1): the terrain heightmap (decimated to keep triangle counts
// sane) plus every static box collider. `map`/`anchor` may describe a terrain
// tile (map null = no terrain).
void collect_nav_geometry(entt::registry& registry, const terrain::Heightmap* map,
                          const glm::vec3& anchor, std::vector<glm::vec3>& verts,
                          std::vector<std::uint32_t>& indices, bool include_colliders = true);

// Tile-level variant for streamed worlds: a static height-field body whose
// minimum XZ corner sits at `origin` (12.4 creates/destroys one per tile).
[[nodiscard]] std::uint32_t add_height_field_body(physics::PhysicsWorld& world,
                                                  const terrain::Heightmap& map,
                                                  const glm::vec3& origin);

// Generates a small tile, adds its height-field body, drops a sphere onto it
// through the ECS physics system, and verifies it rests on the surface height.
[[nodiscard]] std::expected<void, core::Error> run_terrain_physics_self_test();

// Advance every CharacterController: integrate gravity, apply `move` * `speed`
// horizontally, move + resolve collisions via the character controller, and write
// the result back into Transform.
void character_system(entt::registry& registry, physics::PhysicsWorld& world, float dt);

// Drops a character onto a floor, then walks it, verifying it lands (on_ground)
// and moves horizontally.
[[nodiscard]] std::expected<void, core::Error> run_character_self_test();

// Advance every active NavAgent: repath on the navmesh when the target moved,
// steer toward the current waypoint with simple agent-agent separation, and
// either drive the entity's CharacterController (move/speed) or translate the
// Transform kinematically. Call before character_system each tick.
void nav_agent_system(entt::registry& registry, const nav::NavMesh& navmesh, float dt);

// A kinematic agent walks from one side of the walled test plate to the other:
// it must arrive at the target and must have detoured through the gap.
[[nodiscard]] std::expected<void, core::Error> run_nav_agent_self_test();

// Builds a small hierarchy, ticks the scene, and verifies world-matrix
// propagation and draw-list collection. No device needed.
[[nodiscard]] std::expected<void, core::Error> run_scene_self_test();

} // namespace engine::scene
