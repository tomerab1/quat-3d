#pragma once

// Draw batching for instanced rendering (ROADMAP R9.1).
//
// The scene collects one DrawItem per entity. The GBuffer and shadow passes
// draw an InstancedDrawList instead: draws that share a mesh and a material
// collapse into one instanced draw whose per-instance data (model matrix +
// normal matrix) sits in a flat array. The passes upload that array into a
// host-visible buffer per frame slot and the vertex shaders read it through a
// buffer device address (lib/instancing.slang), indexing it with the Vulkan
// instance index — which already includes the draw's firstInstance, so
// `first_instance` is passed as firstInstance and nowhere else.
//
// Everything here is CPU-only and pure (no GPU objects are touched; mesh and
// material are compared by address), so it is unit-testable without a device.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace engine::asset {
struct MeshAsset;
struct MaterialAsset;
} // namespace engine::asset

namespace engine::renderer {

// One thing to draw. `material` may be null — the pass then uses its built-in
// default (white, untextured) material. `skinned_vertices`, when set, overrides
// the mesh's bind-pose vertex buffer with a skinned buffer produced by the
// skinning pass this frame (same asset::Vertex layout).
//
// Velocity (R11, velocity.hpp): `prev_model` is last frame's model matrix
// (unset: same as `model`, camera-only velocity); `prev_positions`, for
// skinned items, addresses last frame's skinned positions (one float4 per
// vertex, written by the skinning pass) in the same space as the vertices.
struct DrawItem {
    const asset::MeshAsset*     mesh     = nullptr;
    const asset::MaterialAsset* material = nullptr;
    glm::mat4                   model{1.0F};
    VkBuffer                    skinned_vertices = VK_NULL_HANDLE;
    std::optional<glm::mat4>    prev_model{};
    VkDeviceAddress             prev_positions = 0;
};

// Per-instance GPU data, mirrors InstanceData in lib/instancing.slang (112
// bytes, 16-byte aligned members, column-major like every glm matrix).
//
// `normal` holds the columns of the normal matrix sign(det M) * cofactor(M),
// M = the model's upper 3x3 — i.e. |det M| * M^-T: correct under non-uniform
// scale, stays finite for singular (flattened) transforms, and keeps normals
// pointing outward under mirroring. normal[0].w carries sign(det M): the
// tangent's bitangent sign is multiplied by it so normal-mapped surfaces keep
// their handedness when mirrored. normal[1].w / normal[2].w are zero.
struct InstanceData {
    glm::mat4                model{1.0F};
    std::array<glm::vec4, 3> normal{glm::vec4(1.0F, 0.0F, 0.0F, 1.0F),
                                    glm::vec4(0.0F, 1.0F, 0.0F, 0.0F),
                                    glm::vec4(0.0F, 0.0F, 1.0F, 0.0F)};
};
static_assert(sizeof(InstanceData) == 112, "must match lib/instancing.slang InstanceData");

// Builds the instance record (model + normal matrix + handedness) for `model`.
[[nodiscard]] InstanceData make_instance_data(const glm::mat4& model);

// Per-instance previous-frame data for the velocity target, mirrors
// InstanceMotion in lib/velocity.slang (64 bytes): last frame's model matrix.
// Last frame's skinned positions are per draw (InstancedDraw::prev_positions):
// the shader must not dereference a pointer read from this array, which
// MoltenVK mistranslates (issue #103, tests/rhi/msl_portability_test.cpp).
struct InstanceMotion {
    glm::mat4 prev_model{1.0F};
};

[[nodiscard]] inline InstanceMotion make_instance_motion(const glm::mat4& prev_model) {
    InstanceMotion m;
    m.prev_model = prev_model;
    return m;
}
static_assert(sizeof(InstanceMotion) == 64, "must match lib/velocity.slang InstanceMotion");

// One instanced draw: `instance_count` instances of `mesh` with `material`,
// reading InstancedDrawList::instances[first_instance ..
// first_instance + instance_count). The same shape serves the automatic
// batching here and explicit multi-placement sources later (R9.2
// InstancedMesh appends its transforms and one InstancedDraw).
// `prev_positions` (DrawItem::prev_positions, 0 = none) belongs to a
// single-instance draw: last frame's positions of exactly its vertices.
struct InstancedDraw {
    const asset::MeshAsset*     mesh     = nullptr;
    const asset::MaterialAsset* material = nullptr;
    VkBuffer                    skinned_vertices = VK_NULL_HANDLE; // see DrawItem
    VkDeviceAddress             prev_positions = 0;                // see DrawItem
    std::uint32_t               first_instance = 0;
    std::uint32_t               instance_count = 0;
};

// Draws plus the flat instance array they index. Every draw's instance range
// lies inside `instances`; ranges of different draws do not overlap.
// `motion` is parallel to `instances` (same size and order) or empty, which
// the GBuffer treats as "no previous-frame data" (camera-only velocity).
struct InstancedDrawList {
    std::vector<InstanceData>   instances;
    std::vector<InstanceMotion> motion;
    std::vector<InstancedDraw>  draws;
};

// Groups `items` by (mesh, material) into instanced draws. Deterministic:
// draws appear in the order their first item appears in `items`, and each
// draw's instances keep the items' relative order. Skinned items (with
// `skinned_vertices` or `prev_positions`) are never merged — each reads its
// own per-vertex buffers — and stay single-instance draws at their position
// in that order.
// Items without a mesh are dropped.
[[nodiscard]] InstancedDrawList batch_draws(std::span<const DrawItem> items);

// One single-instance draw per item, in the items' order (items without a mesh
// are dropped). For order-dependent draws such as back-to-front sorted
// transparent geometry, which must not be regrouped.
[[nodiscard]] InstancedDrawList unbatched_draws(std::span<const DrawItem> items);

// True when the instance's transform mirrors (det < 0): its triangles wind
// the other way on screen (InstanceData::normal[0].w carries sign(det)).
[[nodiscard]] inline bool is_mirrored(const InstanceData& instance) {
    return instance.normal[0].w < 0.0F;
}

// Reorders each draw's instance range so its unmirrored instances come first
// and its mirrored ones last (stable within each group; `motion` follows when
// present) and returns, per draw, how many unmirrored instances lead its
// range. A pass that culls back faces draws the two groups separately, the
// mirrored one culling front faces (scatter.mirror makes such mixes common).
[[nodiscard]] std::vector<std::uint32_t> partition_mirrored(InstancedDrawList& list);

} // namespace engine::renderer
