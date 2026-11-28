#pragma once

// Sweeps, lofts and extrusions (ROADMAP R23.1).
//
// Paths are polylines of 3D points with rotation-minimising frames (Wang,
// Jüttler, Zheng & Liu, "Computation of rotation minimizing frames", ACM
// TOG 2008: the double reflection method), so profiles do not twist along a
// curve. TODO(G18): take engine_spline curves (and their frames) once G18
// (#131) lands; until then callers sample their curve into a polyline.
//
// Profiles are 2D in the frame's (normal, binormal) plane: x along the
// frame normal (sideways, horizontal for a horizontal path), y along the
// binormal (up). A wall's profile is its cross-section.

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/geometry/mesh_data.hpp"
#include "engine/geometry/polygon.hpp"

namespace engine::geometry {

struct PathFrame {
    glm::dvec3 origin{0.0};
    glm::dvec3 tangent{0.0, 0.0, 1.0};  // along the path (at a corner: the bisector)
    glm::dvec3 normal{1.0, 0.0, 0.0};   // profile x
    glm::dvec3 binormal{0.0, 1.0, 0.0}; // profile y
    double distance = 0.0;              // arc length from the start
    // At a corner the profile is stretched by miter_scale = 1 / cos(half
    // the turn) along miter_direction (in the frame's plane, towards the
    // outside of the turn), so straight segments keep their cross-section.
    double miter_scale = 1.0;
    glm::dvec3 miter_direction{0.0};
};

struct FrameOptions {
    glm::dvec3 up{0.0, 1.0, 0.0}; // the first frame's binormal leans towards this
    bool closed = false;          // the path loops (last point joins the first)
};

// Frames at every path point. Consecutive duplicate points are refused, as
// are U-turns (consecutive segments more than 179 degrees apart). A closed
// path's residual twist is spread evenly along its length.
[[nodiscard]] std::expected<std::vector<PathFrame>, core::Error>
path_frames(std::span<const glm::dvec3> path, const FrameOptions& options = {});

struct SweepOptions {
    bool closed_path = false;    // join the last ring to the first
    bool closed_profile = false; // the profile is a loop (a tube), else open (a strip)
    bool cap_start = false;      // closed profiles on open paths only
    bool cap_end = false;
    double miter_limit = 4.0;    // refuse corners sharper than this stretch
    double uv_scale = 1.0;       // metres per UV unit: u along the path, v along the profile
    float crease_angle_degrees = 40.0F;
    std::uint32_t material = 0;
    std::uint32_t cap_material = 0;
};

// Sweeps `profile` along `frames` (from path_frames). Corners are mitred:
// the profile is projected onto the bisecting plane so straight segments
// keep their cross-section. Normals by crease angle, UVs by arc length (u
// along the path, v along the profile), MikkTSpace tangents.
[[nodiscard]] std::expected<MeshData, core::Error> sweep(std::span<const glm::dvec2> profile,
                                                         std::span<const PathFrame> frames,
                                                         const SweepOptions& options = {});

// Lofts between profiles placed on frames: profiles[i] lies on frames[i];
// all profiles have the same point count.
[[nodiscard]] std::expected<MeshData, core::Error> loft(std::span<const std::vector<glm::dvec2>> profiles,
                                                        std::span<const PathFrame> frames,
                                                        const SweepOptions& options = {});

struct ExtrudeOptions {
    double height = 1.0;   // world Y extent (the polygon lies in XZ at base_height)
    double base_height = 0.0;
    double taper = 0.0;    // inset of the top outline (metres; negative flares out)
    bool cap_top = true;
    bool cap_bottom = true;
    double uv_scale = 1.0;
    float crease_angle_degrees = 40.0F;
    std::uint32_t side_material = 0;
    std::uint32_t cap_material = 0;
};

// Extrudes a plan polygon (with holes) upward. With a taper the top outline
// is the mitred inset of the bottom one; an inset that would invert the
// outline (past its first straight-skeleton event) is refused.
[[nodiscard]] std::expected<MeshData, core::Error> extrude(const Polygon& polygon,
                                                           const ExtrudeOptions& options = {});

} // namespace engine::geometry
