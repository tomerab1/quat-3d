#pragma once

// InstancedMesh support (ROADMAP R9.2): resolving the model an InstancedMesh
// draws (InstancedMeshParts) and collecting its placements into the
// renderer's instanced draw lists — one InstancedDraw per (mesh, material)
// across every InstancedMesh entity, never one entity or draw per placement.
//
// A coarse per-instance cull runs here on the CPU against a bounding sphere
// (the union of the model's parts); the passes then cull each view on the GPU
// (R6 / R9.4, renderer/instance_cull_pass.hpp): per shadow cascade, with the
// projected-size and caster-distance (InstancedMesh::shadow_distance) tests.
//
// Frame-loop use (after the scene's transforms are current):
//   append_instanced_transparent_items(reg, cam_vp, transparent_items) // before sorting
//   lists = build_frame_draw_lists(reg, draws, casters, cam_vp, light_vp)
//   (batch_draws + append_instanced_draws for the opaque and shadow passes)
// Alpha-blended and transmissive parts go to the transparent list as
// per-instance DrawItems (they must be depth-sorted, so they cannot share an
// instanced draw); the frame loop already makes transmissive items cast
// shadows, so the shadow pass here collects opaque parts only.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <vector>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/motion_history.hpp"
#include "engine/scene/components.hpp"

namespace engine::asset {
class AssetManager;
}
namespace engine::rhi {
class GpuAllocator;
struct TransferContext;
} // namespace engine::rhi

namespace engine::scene {

// Local matrix of a placement: T(position) * R(rotation) * S(scale).
[[nodiscard]] glm::mat4 instance_matrix(const InstanceTransform& instance);

// Content hash of the fields an InstancedMesh's parts are built from (glTF
// path + node, or the primitive's shape + material). Never 0.
[[nodiscard]] std::uint64_t instanced_mesh_source_hash(const InstancedMesh& mesh);

// Bounding sphere (xyz centre, w radius) of `parts` in the instance frame,
// from each mesh's AABB through its part matrix. Zero radius when empty.
[[nodiscard]] glm::vec4 instanced_parts_bounds(std::span<const InstancedMeshPart> parts);

// True when some InstancedMesh lacks InstancedMeshParts or has stale ones.
// Cheap (no GPU access) — decides whether to open an upload context.
[[nodiscard]] bool instanced_meshes_need_resolve(const entt::registry& registry);

// (Re)builds the InstancedMeshParts of every InstancedMesh whose source
// changed. Primitive sources share the Primitive component's assets
// (load_primitive_assets). glTF sources instantiate the file (relative paths
// resolve against `root`) into a scratch scene — assets are cached by the
// AssetManager — and take every static mesh in the scene, or in the subtree
// of the node named `gltf_node`, with its transform relative to that node
// (skinned meshes are skipped: instances do not animate). Uploads block on
// `transfer`. Returns the number of entities rebuilt; failures (missing
// file / node, upload errors) are reported after every entity is processed,
// and a failed entity keeps empty parts until its source changes.
[[nodiscard]] std::expected<std::size_t, core::Error>
resolve_instanced_meshes(entt::registry& registry, rhi::GpuAllocator& allocator,
                         const rhi::TransferContext& transfer, asset::AssetManager& assets,
                         const std::filesystem::path& root);

enum class InstancedPass : std::uint8_t {
    opaque, // GBuffer: every opaque part
    shadow, // shadow casters: opaque parts of meshes with cast_shadows
};

// Appends the visible placements of every InstancedMesh (with Transform +
// InstancedMeshParts) to `out`: one InstancedDraw per (mesh, material),
// shared across entities, in the order each pair is first met. Instances
// whose bounding sphere lies outside the frustum of `cull_view_proj`
// (Vulkan [0, 1] depth) are dropped. Returns the instances appended.
//
// Velocity (R11): when `out.motion` is parallel to `out.instances`, a motion
// record is appended per instance — each instance's previous model is the
// entity's previous world transform (from `motion`, keyed by the entity id;
// null or unseen: the current one) times its placement.
std::size_t append_instanced_draws(const entt::registry& registry,
                                   const glm::mat4& cull_view_proj, InstancedPass pass,
                                   renderer::InstancedDrawList& out,
                                   renderer::MotionHistory* motion = nullptr);

// Appends one DrawItem per visible placement of every alpha-blended or
// transmissive part (for the forward pass, which sorts them back to front).
// Returns the items appended.
std::size_t append_instanced_transparent_items(const entt::registry& registry,
                                               const glm::mat4& cull_view_proj,
                                               std::vector<renderer::DrawItem>& out);

// The frame's batched opaque (GBuffer) and shadow-caster draw lists: the
// gathered draw items grouped by (mesh, material) (renderer::batch_draws),
// followed by every visible InstancedMesh placement (append_instanced_draws,
// culled against the camera and the shadow casters' volume respectively,
// e.g. renderer::caster_cull_view_proj). `opaque_motion`, when set, records
// each opaque placement's previous transform for the velocity target
// (append_instanced_draws' `motion`).
struct FrameDrawLists {
    renderer::InstancedDrawList opaque;
    renderer::InstancedDrawList shadow;
};
[[nodiscard]] FrameDrawLists build_frame_draw_lists(const entt::registry& registry,
                                                    std::span<const renderer::DrawItem> opaque_items,
                                                    std::span<const renderer::DrawItem> shadow_items,
                                                    const glm::mat4& camera_view_proj,
                                                    const glm::mat4& shadow_cull_view_proj,
                                                    renderer::MotionHistory* opaque_motion = nullptr);

// Draws the frame submits for its scene geometry (debug.stats, the editor's
// Stats panel): one per batched opaque and shadow draw — instanced batches
// included — plus one per transparent item (the forward pass draws those
// unmerged, back to front).
[[nodiscard]] std::size_t frame_draw_count(const FrameDrawLists& lists,
                                           std::size_t transparent_items);

// The same draws with their instance and triangle counts (profiling, R4):
// shadow casters only when `with_shadows` (the cascades are rendered).
struct FrameDrawStats {
    std::size_t   draws = 0;
    std::uint64_t instances = 0;
    std::uint64_t triangles = 0;
};
[[nodiscard]] FrameDrawStats frame_draw_stats(const FrameDrawLists& lists,
                                              std::span<const renderer::DrawItem> transparent,
                                              bool with_shadows);

} // namespace engine::scene
