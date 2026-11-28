#pragma once

// Geometric queries on curves (ROADMAP G18.1): closest point, crossings in
// the ground plane (XZ), offset curves. All deterministic (see spline.hpp).

#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "engine/spline/curve.hpp"

namespace engine::spline {

struct ClosestPoint {
    float     param = 0.0F;    // t on the curve
    float     distance = 0.0F; // arc length to it from the start
    glm::vec3 position{0.0F};
    float     gap = 0.0F;      // distance from the query point
};

// The point of `curve` nearest `p` (in 3D, or in XZ only with `xz_only`:
// picking a curve on the ground from a cursor hit). Segments are visited in
// order of their bounding-box distance and pruned against the best so far;
// each is coarsely sampled, then refined by Newton steps on
// (B(u) - p) . B'(u) = 0 (clamped to the segment; ends checked too).
// nullopt on an empty curve.
[[nodiscard]] std::optional<ClosestPoint> closest_point(const Curve& curve, const glm::vec3& p,
                                                        bool xz_only = false);

// A crossing of two curves seen from above.
struct Crossing {
    float     param_a = 0.0F;
    float     param_b = 0.0F;
    glm::vec3 position_a{0.0F}; // on curve a (its own height)
    glm::vec3 position_b{0.0F}; // on curve b
};

struct CrossingOptions {
    // Flattening tolerance (metres): segments are subdivided until their
    // control polygon is this close to the chord, then each pair of chords is
    // tested and its hit refined on the curves by Newton steps.
    float tolerance = 1.0e-3F;
    // Crossings closer than this along both curves are one crossing.
    float merge_distance = 1.0e-3F;
};

// Every XZ crossing of `a` and `b`, ordered by param_a then param_b. A
// bounding-volume hierarchy over the flattened chords of `b` keeps this
// near-linear. Touching end to end counts as a crossing.
[[nodiscard]] std::vector<Crossing> crossings_xz(const Curve& a, const Curve& b,
                                                 const CrossingOptions& options = {});

// Self-crossings of one curve in XZ (param_a < param_b), excluding the shared
// points of neighbouring segments and, on a closed curve, the seam.
[[nodiscard]] std::vector<Crossing> self_crossings_xz(const Curve& curve, const CrossingOptions& options = {});

// A curve flattened to a polyline within `tolerance` metres (adaptive:
// straight runs stay one chord). Each point carries its curve parameter.
// Segments are subdivided in coordinates relative to their start, and the
// tolerance is at least 8 ulps of the curve's largest coordinate, so far
// from the origin the chords follow the curve, not float rounding. At most
// about 2^20 chords per call (fewer subdivisions per segment on huge curves).
struct PolylinePoint {
    glm::vec3 position{0.0F};
    float     param = 0.0F;
};
[[nodiscard]] std::vector<PolylinePoint> flatten(const Curve& curve, float tolerance);

struct OffsetOptions {
    // Signed distance along the up-locked right vector (+ = right of travel).
    float distance = 0.0F;
    // Added to `distance`: this many of each sample's half widths (1 = the
    // edge of a path of the curve's width; -1 its left edge).
    float width_factor = 0.0F;
    // Arc-length spacing of the offset samples (metres).
    float spacing = 0.25F;
    // Remove what comes closer to the curve than the offset distance: the
    // swallowtails inside tight bends and a U-turn's overlapping inner side,
    // joining the remaining runs where they cross.
    bool  trim_loops = true;
    std::size_t max_points = 1U << 16;
};

// The offset of `curve` in XZ as a polyline (y follows the curve). A closed
// curve gives a closed polyline (the first point is not repeated; trimming
// may start it at another point). Empty when nothing of it is valid.
[[nodiscard]] std::vector<glm::vec3> offset_xz(const Curve& curve, const OffsetOptions& options);

} // namespace engine::spline
