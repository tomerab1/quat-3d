#pragma once

// NavMesh — Recast/Detour navigation mesh (Phase 13, Slice 13.1).
//
// build() runs the standard Recast pipeline over a triangle soup of walkable
// world geometry (voxelize -> filter -> regions -> contours -> poly mesh) and
// bakes a Detour navmesh + query object. find_path() returns a straightened
// waypoint list between two world positions snapped to the mesh. Recast and
// Detour are implementation details (PIMPL) — consumers see glm only.

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"

namespace engine::nav {

struct NavMeshParams {
    float cell_size = 0.3F;          // voxel size in the ground plane, metres
    float cell_height = 0.2F;        // voxel height, metres
    float agent_height = 1.8F;       // walkable clearance
    float agent_radius = 0.4F;       // walkable surfaces erode by this
    float agent_max_climb = 0.5F;    // step height
    float agent_max_slope_deg = 50.0F;
};

// A planned route. `partial` means Detour could not reach the target's
// polygon (unreachable, or the corridor buffer filled on a very long route):
// the points end at the closest reachable spot instead.
struct NavPath {
    std::vector<glm::vec3> points;
    bool                   partial = false;
};

class NavMesh {
public:
    // Endpoint snapping box (half-extents, metres): generous vertically so
    // agent pivots above their feet and targets on coarse terrain bakes
    // (whose mesh floats off the true surface) still find their polygon.
    static constexpr glm::vec3 query_extents{4.0F, 16.0F, 4.0F};

    // Builds from world-space triangles. Winding matters: Recast takes each
    // triangle's normal as (v1 - v0) x (v2 - v0) and only marks triangles
    // whose normal points up within the slope limit as walkable, so walkable
    // surfaces must be counter-clockwise seen from above (outward-facing for
    // closed shapes).
    [[nodiscard]] static std::expected<NavMesh, core::Error>
    build(std::span<const glm::vec3> vertices, std::span<const std::uint32_t> indices,
          const NavMeshParams& params = {});

    NavMesh();
    NavMesh(NavMesh&&) noexcept;
    NavMesh& operator=(NavMesh&&) noexcept;
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;
    ~NavMesh();

    [[nodiscard]] bool valid() const;

    // Straightened (string-pulled) path between two world points, both snapped
    // to the nearest mesh polygon within query_extents. Errors when either
    // end is off-mesh; an unreachable target yields a partial path.
    [[nodiscard]] std::expected<NavPath, core::Error>
    plan_path(const glm::vec3& start, const glm::vec3& end) const;

    // plan_path's points only (partial paths included).
    [[nodiscard]] std::expected<std::vector<glm::vec3>, core::Error>
    find_path(const glm::vec3& start, const glm::vec3& end) const;

    // Nearest point ON the mesh to `pos` (agent spawning/clamping).
    [[nodiscard]] std::expected<glm::vec3, core::Error>
    nearest_point(const glm::vec3& pos) const;

    // Height of the walkable surface at (x, z), searching the whole vertical
    // extent of the mesh: of several stacked surfaces (a bridge over a
    // valley) the one closest to `reference_y` wins. Off the mesh in xz, the
    // nearest polygon within query_extents horizontally is used. Errors
    // when there is no polygon near (x, z) at all.
    [[nodiscard]] std::expected<float, core::Error>
    surface_height(float x, float z, float reference_y) const;

    // Polygon edges of the baked mesh, for the editor overlay.
    [[nodiscard]] const std::vector<std::pair<glm::vec3, glm::vec3>>& debug_edges() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Self-test: a flat plate with a wall across the middle (gap at one side) —
// the path from one half to the other exists, stays on the plate, and routes
// through the gap rather than the wall.
[[nodiscard]] std::expected<void, core::Error> run_navmesh_self_test();

} // namespace engine::nav
