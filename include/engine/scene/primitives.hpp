#pragma once

// Procedural mesh generators (CPU-side MeshData, upload with upload_mesh()) and
// the Primitive component's resolution into a MeshRenderer. Used by the demo
// scenes, the tests, and remote/AI world building.
//
// Generator conventions (shared by every shape, matching glTF): triangles wind
// counter-clockwise seen from outside, normals are unit length, tangent.xyz
// follows +U and the bitangent cross(normal, tangent.xyz) * tangent.w follows
// +V, so normal maps sample consistently across shapes.

#include <cstdint>
#include <expected>
#include <string>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/asset/mesh_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp" // MeshData

namespace engine::asset {
class AssetManager;
}
namespace engine::rhi {
class GpuAllocator;
struct TransferContext;
} // namespace engine::rhi

namespace engine::scene {

// Unit cube centred at the origin, per-face normals (24 vertices, 36 indices).
[[nodiscard]] MeshData make_cube_data();

// Box with the given half-extents baked into the geometry (no Transform scale
// needed — keeps physics rotation extraction clean).
[[nodiscard]] MeshData make_box_data(const glm::vec3& half_extents);

// UV sphere (latitude/longitude grid) of `radius`: (rings+1)*(sectors+1)
// vertices, rings*sectors*6 indices (the pole rows carry degenerate triangles).
[[nodiscard]] MeshData make_sphere_data(float radius, std::uint32_t rings, std::uint32_t sectors);

// Square of side `size` in the XZ plane facing +Y (4 vertices, 6 indices).
[[nodiscard]] MeshData make_plane_data(float size);

// Capped cylinder along Y, centred on the origin: smooth side plus two flat
// caps with their own vertices (hard edges at the rims).
[[nodiscard]] MeshData make_cylinder_data(float radius, float height, std::uint32_t sectors);

// Capped cone along Y: base disc at -height/2, apex at +height/2. The apex is
// split per sector so each side facet gets a well-defined normal.
[[nodiscard]] MeshData make_cone_data(float radius, float height, std::uint32_t sectors);

// Capsule along Y: a cylinder of `cylinder_height` between two hemispheres of
// `radius` (total height cylinder_height + 2 * radius). `hemisphere_rings`
// latitude bands per cap.
[[nodiscard]] MeshData make_capsule_data(float radius, float cylinder_height,
                                         std::uint32_t hemisphere_rings, std::uint32_t sectors);

// ---------------------------------------------------------------------------
// Primitive component support
// ---------------------------------------------------------------------------

// The unit mesh of a Primitive shape, sized as documented on Primitive (cube
// 1 m, sphere / cylinder / cone / capsule 1 m diameter, cylinder and cone 1 m
// tall, capsule 2 m tall, plane 1x1 m facing +Y).
[[nodiscard]] MeshData make_primitive_data(PrimitiveShape shape);

// Local-space bounds of a Primitive shape's unit mesh (no GPU access needed).
[[nodiscard]] asset::Aabb primitive_bounds(PrimitiveShape shape);

// Content hash of the fields a Primitive's MeshRenderer is built from (shape +
// material). Never 0, so a freshly decoded Primitive (resolved == 0) always
// resolves.
[[nodiscard]] std::uint64_t primitive_hash(const Primitive& primitive);

// The flat PBR material a Primitive renders with: its colour (alpha < 1 ->
// alpha-blended unless a material shader is set), metallic/roughness,
// emission and shader parameters; planes are double-sided. (The shader name
// itself goes on the MaterialAsset, see load_primitive_assets.)
[[nodiscard]] asset::PbrMaterialParams primitive_material_params(const Primitive& primitive);

// True when some Primitive's MeshRenderer is missing or stale. Cheap (no GPU
// access) — the frame loop calls it to decide whether to open an upload
// context for resolve_primitives().
[[nodiscard]] bool primitives_need_resolve(const entt::registry& registry);

// The shared mesh + material a Primitive renders with, loaded (or found)
// through `assets` under the keys described at resolve_primitives(). A failed
// upload leaves the handle in the failed state (it renders the manager's
// default); the keys name what failed.
struct PrimitiveAssets {
    asset::AssetHandle<asset::MeshAsset>     mesh;
    asset::AssetHandle<asset::MaterialAsset> material;
    std::string                              mesh_key;
    std::string                              material_key;
};
[[nodiscard]] PrimitiveAssets load_primitive_assets(const Primitive& primitive,
                                                    rhi::GpuAllocator& allocator,
                                                    const rhi::TransferContext& transfer,
                                                    asset::AssetManager& assets);

// (Re)builds the MeshRenderer of every Primitive whose `resolved` hash differs
// from its current fields. Meshes and materials are shared through `assets`:
// meshes under "primitive/mesh/<shape>", materials under
// "primitive/material/<16 hex digits>" (a hash of the material parameters and
// material shader), so
// a thousand identical blocks cost one mesh and one material. Uploads block on
// `transfer`. Returns the number of entities rebuilt; an asset that fails to
// upload is reported after every entity has been processed (the entity then
// renders with the manager's default asset and is not retried).
[[nodiscard]] std::expected<std::size_t, core::Error>
resolve_primitives(entt::registry& registry, rhi::GpuAllocator& allocator,
                   const rhi::TransferContext& transfer, asset::AssetManager& assets);

} // namespace engine::scene
