#pragma once

// Bounds of what entities draw (ROADMAP A9): the boxes the view camera's clip
// range, camera framing, remote `scene.get` bounds and `world.place_on_ground`
// are built from. CPU only (mesh bounds come from the loaded MeshAsset, so no
// GPU access is needed).
//
// An entity draws a MeshRenderer (a glTF mesh or a resolved Primitive), a
// Primitive whose mesh is not resolved yet (its unit shape), or an
// InstancedMesh (every placement of its model). Terrain is runtime state
// (streamed heightmaps), not an entity drawable: callers add it themselves.

#include <optional>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/asset/mesh_asset.hpp" // Aabb

namespace engine::scene {

// An empty box: merging anything into it yields that thing.
[[nodiscard]] asset::Aabb empty_bounds();
[[nodiscard]] bool bounds_empty(const asset::Aabb& box);
// Grows `into` to contain `box` (an empty `box` changes nothing).
void merge_bounds(asset::Aabb& into, const asset::Aabb& box);
void merge_point(asset::Aabb& into, const glm::vec3& p);
// The axis-aligned box around `box`'s eight corners under `m` (exact for any
// affine `m`: rotation, non-uniform scale, shear).
[[nodiscard]] asset::Aabb transform_bounds(const asset::Aabb& box, const glm::mat4& m);

// What `e` itself draws, in its own local (model) space: the space its
// Transform::world maps to the world. Skinned meshes are placed through their
// armature (SkinnedMesh::root_transform), as the renderer does. nullopt when
// the entity draws nothing yet (no drawable component, a mesh still loading,
// an instanced glTF model not resolved, no instances).
[[nodiscard]] std::optional<asset::Aabb> local_draw_bounds(const entt::registry& registry,
                                                           entt::entity e);

// World-space bounds of everything in the scene that draws, from each
// entity's Transform::world (tick the scene, or refresh edited transforms,
// first). nullopt for a scene that draws nothing.
[[nodiscard]] std::optional<asset::Aabb> scene_draw_bounds(const entt::registry& registry);

} // namespace engine::scene
