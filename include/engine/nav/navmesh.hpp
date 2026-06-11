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

class NavMesh {
public:
    // Builds from world-space triangles (CCW or CW; Recast filters by slope).
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
    // to the nearest mesh polygon within a few metres. Errors when either end
    // is off-mesh or no route exists.
    [[nodiscard]] std::expected<std::vector<glm::vec3>, core::Error>
    find_path(const glm::vec3& start, const glm::vec3& end) const;

    // Nearest point ON the mesh to `pos` (agent spawning/clamping).
    [[nodiscard]] std::expected<glm::vec3, core::Error>
    nearest_point(const glm::vec3& pos) const;

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
