#pragma once

// Weighted straight skeleton and roofs (ROADMAP R23.1).
//
// The straight skeleton (Aichholzer, Aurenhammer, Alberts & Gärtner, "A
// novel type of skeleton for polygons", J.UCS 1995) is traced by the
// wavefront: every polygon edge moves inward, parallel to itself, at a speed
// equal to its weight, and the skeleton is the set of paths its vertices
// trace. Read as a terrain (height = time), each edge sweeps one planar roof
// face; a weight is the cotangent of that face's pitch, and weight 0 keeps
// the edge in place: a vertical gable (Kelly & Wonka, "Interactive
// architectural modeling with procedural extrusions", ACM TOG 2011).
//
// The event simulation follows Felkel & Obdržálek ("Straight skeleton
// implementation", SCCG 1998): edge events and split events in time order,
// with vertex velocities derived from the moving supporting lines so that
// weights need no special case. Simultaneous events at one point (vertex
// events, and the many coincidences of rectilinear footprints) are resolved
// together as one generalised intersection event (Kelly & Wonka 2011, §4),
// following the robustness lessons of Eder, Held & Palfrader ("On
// implementing straight skeletons: challenges and experiences", SoCG 2020).
// Configurations whose weighted skeleton is ambiguous (Biedl, Held, Huber,
// Kaaser & Palfrader, "Weighted straight skeletons in the plane", CGTA 2015)
// — two adjacent parallel wavefront edges with different weights — are
// refused with an error rather than guessed.

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/geometry/mesh_data.hpp"
#include "engine/geometry/polygon.hpp"

namespace engine::geometry {

struct SkeletonNode {
    glm::dvec2 position{0.0};
    double time = 0.0; // offset distance at unit weight = roof height at pitch 45 deg
};

// A skeleton arc between two nodes. `left_edge` / `right_edge` are the input
// edges whose faces lie left / right of the direction from -> to.
struct SkeletonArc {
    std::uint32_t from = 0;
    std::uint32_t to = 0;
    std::uint32_t left_edge = 0;
    std::uint32_t right_edge = 0;
};

// The face swept by one input edge: node indices counter-clockwise, starting
// with the edge's two input vertices.
struct SkeletonFace {
    std::uint32_t edge = 0;
    std::vector<std::uint32_t> nodes;
};

struct StraightSkeleton {
    // Nodes 0..n-1 are the polygon's vertices after clean-up (outer ring,
    // then holes; duplicates dropped, collinear equal-weight edges merged);
    // skeleton edge i runs from node i to the next vertex of its ring.
    std::vector<SkeletonNode> nodes;
    std::vector<SkeletonArc> arcs;
    std::vector<SkeletonFace> faces;          // one per skeleton edge, in edge order
    std::vector<double> edge_weights;         // one per skeleton edge
    std::vector<std::uint32_t> source_edges;  // input edge index of each skeleton edge
    double max_time = 0.0;
};

// Computes the weighted straight skeleton of `polygon` (outer ring + holes).
// `weights` has one entry per edge (outer ring edges first, then each hole's)
// or is empty for all 1. Weights must be finite and >= 0, and not all zero
// on any ring. Collinear consecutive vertices with equal weights are merged
// first; nodes then refer to the merged polygon's vertices.
[[nodiscard]] std::expected<StraightSkeleton, core::Error>
straight_skeleton(const Polygon& polygon, std::span<const double> weights = {});

struct RoofOptions {
    double base_height = 0.0;     // world height of the eaves (polygon plane)
    double height_scale = 1.0;    // roof height per unit skeleton time
    double max_height = 0.0;      // > 0: cut the roof flat at this height above the base
    double uv_scale = 1.0;        // metres per UV unit (UV: along the edge, up the slope)
    std::uint32_t material = 0;        // sloped faces and the flat top
    std::uint32_t gable_material = 0;  // vertical faces of weight-0 edges
};

// Builds the roof surface of a skeleton: one planar face per input edge
// (weight-0 edges give vertical gables), triangulated, with flat normals
// facing out of the roof, per-face UVs (u along the eave, v up the slope, in
// metres / uv_scale) and MikkTSpace tangents. The plan lies in the world XZ
// plane: plan point (x, y) at height h becomes (x, base_height + h, y).
[[nodiscard]] std::expected<MeshData, core::Error> roof_mesh(const StraightSkeleton& skeleton,
                                                             const RoofOptions& options = {});

} // namespace engine::geometry
