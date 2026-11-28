#pragma once

// MeshData processing (ROADMAP R23.1): welding, normals, UVs, tangents and
// GPU-friendly ordering. All functions are deterministic (see mesh_data.hpp)
// and keep submesh ranges valid.

#include <cstdint>
#include <expected>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/geometry/mesh_data.hpp"

namespace engine::geometry {

struct WeldOptions {
    // Positions closer than this (snapped to a grid of this size) merge;
    // 0 merges only bit-identical positions. Other attributes must match
    // exactly.
    float position_tolerance = 0.0F;
    // Ignore normals, UVs and tangents (merge by position only, keeping the
    // first vertex's attributes): for topology before computing normals.
    bool position_only = false;
};

// Merges duplicate vertices and drops triangles that become degenerate
// (two equal indices).
void weld(MeshData& mesh, const WeldOptions& options = {});

struct NormalOptions {
    // Faces meeting at an angle above this keep separate normals (a hard
    // edge); below it the normal is shared (smooth). 0 = flat shading,
    // 180 = smooth everywhere.
    float crease_angle_degrees = 30.0F;
};

// Vertex normals from the triangles around each position, weighted by the
// corner angle (Thürmer & Wüthrich, "Computing vertex normals from
// polygonal meshes", JGT 1998), split along creases. Vertices that share a
// position but differ in UV (seams) still get the same normal.
void compute_normals(MeshData& mesh, const NormalOptions& options = {});

enum class UvProjection : std::uint8_t {
    box,    // per triangle, onto the plane of its dominant normal axis
    planar, // onto the plane through `origin` with normal `axis`
};

struct UvOptions {
    UvProjection projection = UvProjection::box;
    float scale = 1.0F; // metres per UV unit
    glm::vec3 origin{0.0F};
    glm::vec3 axis{0.0F, 1.0F, 0.0F}; // planar: projection normal
};

// Replaces UVs by a projection of the positions (splits vertices where
// adjacent triangles project differently).
void generate_uvs(MeshData& mesh, const UvOptions& options = {});

// MikkTSpace tangents (Mikkelsen, "Simulation of wrinkled surfaces
// revisited", 2008; the glTF 2.0 tangent convention), split where
// triangles disagree. Needs normals and UVs.
[[nodiscard]] std::expected<void, core::Error> compute_tangents(MeshData& mesh);

// Reorders triangles for the post-transform vertex cache (Forsyth's
// algorithm as implemented by meshoptimizer) within each submesh, then
// vertices for fetch locality. Rendering is unchanged.
void optimize(MeshData& mesh);

} // namespace engine::geometry
