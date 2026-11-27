#pragma once

// Clustered light culling — shared data layout + CPU reference (ROADMAP R10.1).
//
// The view frustum is split into a fixed grid of froxels: cluster_grid_x ×
// cluster_grid_y tiles in NDC (so the grid is resolution-independent) times
// cluster_grid_z depth slices spaced exponentially between the camera's near
// and far planes (slice k spans near·(far/near)^(k/Z) .. near·(far/near)^((k+1)/Z),
// so slices stay roughly cubic in view space). The light-cull compute pass
// (light_cull.slang) tests every light's bounding sphere against each
// cluster's view-space AABB and writes a compact per-cluster index list; the
// deferred lighting and forward transparent passes then loop only over the
// lights of the cluster their sample falls in.
//
// This header has no Vulkan dependency: it defines the GPU record layouts
// (mirrored by assets/shaders/lib/clustered_lights.slang) and a pure CPU
// reference of the cluster assignment that the GPU pass is tested against.

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"

namespace engine::renderer {

// Cluster grid dimensions: 16×9 NDC tiles (square-ish tiles at 16:9) × 24
// exponential depth slices = 3456 clusters. Must match light_cull.slang's
// dispatch (the shader reads them from ClusterParams.dims).
inline constexpr std::uint32_t cluster_grid_x = 16;
inline constexpr std::uint32_t cluster_grid_y = 9;
inline constexpr std::uint32_t cluster_grid_z = 24;
inline constexpr std::uint32_t cluster_count = cluster_grid_x * cluster_grid_y * cluster_grid_z;

// Lights uploaded per frame (the light buffer is sized from this).
inline constexpr std::uint32_t max_lights = 4096;

// Capacity of the compact cluster→light index list: an average of 64 lights
// per cluster (884 KiB). Each cluster takes exactly its count from a global
// atomic cursor, so memory follows the scene's average light density rather
// than the worst cluster (a fixed per-cluster slot array would need
// cluster_count × max-per-cluster entries). If a frame ever asks for more,
// the clusters that come last keep a truncated list (the pass also reports
// the requested total so it can be detected).
inline constexpr std::uint32_t max_cluster_light_indices = cluster_count * 64;

// Light kinds sharing one record format (GpuLight::type).
enum class LightType : std::uint32_t {
    point = 0,
    spot = 1, // cone attenuation per KHR_lights_punctual (spot_scale_offset)
};

// One light, GPU layout (std430, 64 bytes; mirrors GpuLight in
// clustered_lights.slang). Spot lights use the same record: `direction`
// is the direction the cone points (world space, unit) and
// spot_scale_offset.xy the KHR_lights_punctual angular attenuation
// (scale = 1 / max(cos_inner - cos_outer, 1e-4), offset = -cos_outer · scale).
// The culling bound is always the sphere (position, range).
struct GpuLight {
    glm::vec4     position_range{0.0F, 0.0F, 0.0F, 10.0F}; // xyz world position, w range
    glm::vec4     color_intensity{1.0F, 1.0F, 1.0F, 1.0F}; // rgb colour, w intensity
    glm::vec3     direction{0.0F, 0.0F, -1.0F};            // spot axis (unused for points)
    std::uint32_t type = static_cast<std::uint32_t>(LightType::point);
    glm::vec4     spot_scale_offset{0.0F};                 // x scale, y offset, zw reserved
};
static_assert(sizeof(GpuLight) == 64, "must match clustered_lights.slang GpuLight");

[[nodiscard]] GpuLight make_point_light(const glm::vec3& position, float range,
                                        const glm::vec3& color, float intensity);

// A spot light pointing along `direction` with full intensity inside
// `inner_cone` and none outside `outer_cone` (half-angles in radians,
// KHR_lights_punctual semantics; inner is clamped below outer).
[[nodiscard]] GpuLight make_spot_light(const glm::vec3& position, const glm::vec3& direction,
                                       float range, float inner_cone, float outer_cone,
                                       const glm::vec3& color, float intensity);

// Per-frame cluster parameters, uniform-buffer layout (std140, 240 bytes;
// mirrors ClusterParams in clustered_lights.slang). Matrices are glm
// column-major (the shader reads them as four column vectors).
struct ClusterParams {
    glm::mat4  view{1.0F};     // world -> view
    glm::mat4  proj{1.0F};     // view -> clip: the projection the grid is built on
    glm::mat4  inv_proj{1.0F}; // clip -> view
    glm::uvec4 dims{cluster_grid_x, cluster_grid_y, cluster_grid_z, 0}; // w = light count
    // x near, y far, z = slices / ln(far/near), w = ln(near) · z, so
    // slice(d) = floor(ln(d) · z - w).
    glm::vec4  depth{0.1F, 1000.0F, 0.0F, 0.0F};
    glm::uvec4 limits{max_cluster_light_indices, 0, 0, 0}; // x = index-list capacity
};
static_assert(sizeof(ClusterParams) == 240, "must match clustered_lights.slang ClusterParams");

// Builds the cluster parameters for a camera. `proj` must be a perspective
// projection ([0,1] depth, standard or reversed Z, any Y flip / off-centre /
// jitter); near and far are recovered from it. Fails for orthographic or
// infinite-far projections, which the exponential slicing cannot represent.
[[nodiscard]] std::expected<ClusterParams, core::Error>
make_cluster_params(const glm::mat4& view, const glm::mat4& proj, std::uint32_t light_count);

// Linear cluster index of grid coordinate `c` (x fastest, then y, then z).
[[nodiscard]] constexpr std::uint32_t cluster_index(const glm::uvec3& c) {
    return (c.z * cluster_grid_y + c.y) * cluster_grid_x + c.x;
}

// Depth slice of a positive view-space distance `view_depth`, clamped to the
// grid (points nearer than near / beyond far fall in the first / last slice).
[[nodiscard]] std::uint32_t depth_slice(const ClusterParams& params, float view_depth);

// View-space distance of slice boundary `k` (0 = near, cluster_grid_z = far).
[[nodiscard]] float slice_boundary(const ClusterParams& params, std::uint32_t k);

// Cluster containing a view-space position (camera looks down -Z), clamped
// to the grid.
[[nodiscard]] glm::uvec3 cluster_of_view_position(const ClusterParams& params,
                                                  const glm::vec3& view_pos);

struct ClusterAabb {
    glm::vec3 min{0.0F};
    glm::vec3 max{0.0F};
};

// Conservative view-space AABB of cluster `c`: the box around the eight
// corners where the tile's four corner rays cross the slice's near and far
// depths (the froxel is the convex hull of those corners).
[[nodiscard]] ClusterAabb cluster_aabb(const ClusterParams& params, const glm::uvec3& c);

// Squared distance from `p` to the box (0 inside).
[[nodiscard]] float aabb_distance_sq(const ClusterAabb& box, const glm::vec3& p);

// Whether a light's culling sphere (view-space centre, range) overlaps the box.
[[nodiscard]] bool sphere_overlaps_aabb(const ClusterAabb& box, const glm::vec3& centre,
                                        float radius);

// CPU reference of the light-cull pass: for every cluster, the indices of the
// lights whose culling sphere overlaps it, ascending. `params.dims.w` is
// ignored (all of `lights` are assigned). Lights with a non-positive range
// touch no cluster.
[[nodiscard]] std::vector<std::vector<std::uint32_t>>
assign_lights_to_clusters(const ClusterParams& params, std::span<const GpuLight> lights);

} // namespace engine::renderer
