#pragma once

// Per-view instance culling (ROADMAP R6 / R9.4) — the data contract shared by
// the GPU cull pass (instance_cull.slang, InstanceCuller) and its CPU
// reference here, which the tests compare the GPU against.
//
// The frame's instanced draw list is culled on the GPU once per view (the
// camera, and each sun-shadow cascade). Every instance of every draw is
// tested against the view; the survivors' instance indices are compacted
// into the view's visible list, inside the draw's own range
// [first_instance, first_instance + instance_count), and each submesh of the
// draw gets a VkDrawIndexedIndirectCommand whose firstInstance is the start
// of that range and whose instanceCount is the number of survivors. Vertex
// shaders read `instances[visible[InstanceIndex]]` (lib/instancing.slang).
//
// The tests, per instance and view (cull_accepts):
//   * the instance's world bounding sphere (the mesh's object-space sphere
//     through the instance's model matrix, radius scaled by the largest axis
//     scale) against the view's six inward planes. A cascade's planes come
//     from its fitted view-projection (computed on the GPU by csm_setup, so
//     the cascade views are only resolvable on the GPU in a live frame); its
//     near plane is pushed toward the sun by the cascade's caster reach
//     (GpuCascades::limits.y), the volume a pancaked caster can shadow from.
//   * shadow views with a receiver camera: the caster's shadow volume (its
//     sphere swept away from the sun across the cascade's depth) must reach
//     the slice of the camera frustum the cascade shades (its split range,
//     widened by the blend band into the previous cascade, and a filter
//     margin). Foliage around the camera no longer goes into the coarse
//     cascades whose slices start tens of metres away, although their
//     squares contain it. Exact for every receiver the camera can see (the
//     per-plane test of a swept sphere against a convex volume is
//     conservative); after Bittner et al. 2011, "Shadow Caster Culling for
//     Efficient Shadow Mapping" (I3D), the receiver-volume form without the
//     visibility mask;
//   * shadow views: the projected-size cull — a caster whose bounding
//     diameter covers fewer than `min_caster_texels` shadow texels of the
//     cascade is dropped (small foliage stops casting into far cascades,
//     where it would cover less than a texel and only alias);
//   * shadow views: the draw's caster distance (InstancedMesh::shadow_distance)
//     — a caster farther than that from the camera does not cast.
// Draws flagged never_cull (skinned meshes, whose bind-pose bounds do not
// hold the animated pose) pass every test.
//
// Plane extraction: Gribb & Hartmann 2001, "Fast Extraction of Viewing
// Frustum Planes from the World-View-Projection Matrix". The pass structure
// (compaction into per-draw ranges, indirect arguments per view) follows
// Haar & Aaltonen 2015, "GPU-Driven Rendering Pipelines" (SIGGRAPH Advances).

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "engine/renderer/csm.hpp"

namespace engine::renderer {

// The camera plus one view per cascade.
inline constexpr std::uint32_t max_cull_views = 1 + max_cascades;
// Instances per cull workgroup (one chunk of one draw per workgroup).
inline constexpr std::uint32_t cull_chunk_size = 64;

// One draw as the cull shader reads it; mirrors CullDraw in
// instance_cull.slang (48 bytes, std430).
struct CullDrawGpu {
    glm::vec4     local_sphere{0.0F}; // object-space bounds: xyz centre, w radius
    std::uint32_t first_instance = 0; // instance array index = base of its compacted range
    std::uint32_t instance_count = 0;
    std::uint32_t first_command = 0;  // into each view's command array
    std::uint32_t command_count = 0;  // one per submesh
    float         shadow_distance = 0.0F; // shadow views: max caster distance, 0 = unlimited
    std::uint32_t flags = 0;              // cull_draw_* bits
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};
static_assert(sizeof(CullDrawGpu) == 48, "must match instance_cull.slang CullDraw");

inline constexpr std::uint32_t cull_draw_never_cull = 1U; // always visible in every view

enum class CullViewKind : std::uint32_t {
    planes = 0,  // the view's own planes (camera)
    cascade = 1, // sun-shadow cascade `cascade` of the GpuCascades buffer
    all = 2,     // keep everything (culling disabled)
};

// One view; mirrors CullView in instance_cull.slang (112 bytes, std140).
struct CullViewGpu {
    std::array<glm::vec4, 6> planes{}; // kind planes: inward, normalised (world units)
    std::uint32_t kind = 0;            // CullViewKind
    std::uint32_t cascade = 0;         // kind cascade: which cascade
    float         min_caster_texels = 0.0F; // kind cascade: projected-size threshold (diameter)
    std::uint32_t pad = 0;
};
static_assert(sizeof(CullViewGpu) == 112, "must match instance_cull.slang CullView");

// The cull dispatch's constants; mirrors CullParams in instance_cull.slang
// (std140, 608 bytes).
struct CullParamsGpu {
    glm::vec4     camera{0.0F}; // xyz camera position (caster distance)
    std::uint32_t view_count = 0;
    std::uint32_t chunk_count = 0;
    std::uint32_t draw_count = 0;
    std::uint32_t command_count = 0; // per view
    std::uint32_t instance_count = 0; // per view (visible list stride)
    std::uint32_t groups_x = 0;       // dispatch width (chunks fold into y beyond it)
    std::uint32_t has_receiver = 0;   // 1: cascade views test the receiver slices below
    std::uint32_t pad1 = 0;
    // The camera whose pixels receive the shadows: its four side planes
    // (inward, normalised) and its view depth as a plane (xyz forward, w
    // offset: depth(x) = dot(xyz, x) + w), the distance the cascades split.
    std::array<glm::vec4, 4> receiver_sides{};
    glm::vec4     receiver_depth{0.0F};
    std::array<CullViewGpu, max_cull_views> views{};
};
static_assert(sizeof(CullParamsGpu) == 688, "must match instance_cull.slang CullParams");

// The receiver camera of shadow views (CullParamsGpu::receiver_*), from its
// unjittered view and projection.
struct CullReceiver {
    std::array<glm::vec4, 4> sides{};
    glm::vec4                depth{0.0F};
};
[[nodiscard]] CullReceiver cull_receiver(const glm::mat4& view, const glm::mat4& projection);

// A view resolved to what the per-instance test needs (CPU reference; the
// shader resolves cascade views the same way from the GPU-fitted buffer).
struct ResolvedCullView {
    std::array<glm::vec4, 6> planes{}; // inward, normalised; [4] is the near plane
    float near_extrude = 0.0F;  // the near plane's slack (caster reach toward the sun)
    float texel_world = 0.0F;   // shadow texel size, 0 = no projected-size cull
    float min_caster_texels = 0.0F;
    bool  shadow = false;       // apply the draws' caster distance
    bool  keep_all = false;
    // Receiver slice (cascade views with a receiver camera).
    bool      has_receiver = false;
    CullReceiver receiver{};
    glm::vec2 slice{0.0F};      // view-depth range the cascade shades
    glm::vec3 away{0.0F};       // unit direction away from the sun
    float     sweep = 0.0F;     // how far a shadow can reach across the cascade
    float     margin = 0.0F;    // filter footprint around a receiver, world units
};

// Gribb-Hartmann planes of `view_proj` (Vulkan [0, 1] depth), inward,
// normalised: left, right, bottom, top, near, far.
[[nodiscard]] std::array<glm::vec4, 6> cull_frustum_planes(const glm::mat4& view_proj);

// The camera view.
[[nodiscard]] CullViewGpu camera_cull_view(const glm::mat4& view_proj);
// Cascade `cascade` of the GPU cascade buffer (planes resolved on the GPU).
[[nodiscard]] CullViewGpu cascade_cull_view(std::uint32_t cascade, float min_caster_texels);
// Everything visible.
[[nodiscard]] CullViewGpu keep_all_cull_view();

// Resolves `view` (cascade views against `cascades`, and `receiver`'s
// slices when given) for the CPU reference.
[[nodiscard]] ResolvedCullView resolve_cull_view(const CullViewGpu& view,
                                                 const GpuCascades& cascades,
                                                 const CullReceiver* receiver = nullptr);

// World bounding sphere (xyz centre, w radius) of `local` through `model`.
[[nodiscard]] glm::vec4 world_sphere(const glm::mat4& model, const glm::vec4& local);

// Whether an instance with world sphere `sphere` of a draw survives `view`.
[[nodiscard]] bool cull_accepts(const ResolvedCullView& view, const CullDrawGpu& draw,
                                const glm::vec4& sphere, const glm::vec3& camera);

// CPU reference of the whole pass: per view, per draw, the ascending
// instance-array indices that survive (the GPU fills the same set in
// arbitrary order). `models[i]` is instance i's model matrix.
[[nodiscard]] std::vector<std::vector<std::vector<std::uint32_t>>>
reference_cull(std::span<const CullViewGpu> views, const GpuCascades& cascades,
               std::span<const CullDrawGpu> draws, std::span<const glm::mat4> models,
               const glm::vec3& camera, const CullReceiver* receiver = nullptr);

// Shadow-filter margin around a receiver, in shadow texels of the cascade
// (PCF / PCSS kernel, normal offset, blocker search), used by the
// receiver-slice test.
inline constexpr float cull_receiver_margin_texels = 32.0F;

} // namespace engine::renderer
