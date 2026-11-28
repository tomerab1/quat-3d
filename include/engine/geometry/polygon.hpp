#pragma once

// 2D polygons (ROADMAP R23.1): offset, booleans, simplification and repair.
//
// Built on Clipper2 (Angus Johnson, Boost licence), which works on integer
// coordinates and is robust for that reason: input is snapped to a grid of
// 1 / kPolygonScale metres (2^-16 m, about 15 µm), the operation runs on
// int64 coordinates, and the result is converted back. Coordinates must lie
// within ±kPolygonMaxCoordinate.
//
// Conventions: outer rings are counter-clockwise, holes clockwise (y up),
// rings are implicitly closed (the last point is not repeated). Results are
// canonical: each output polygon is an outer ring with its holes, ordered
// deterministically.
//
// Determinism: integer arithmetic is exact; intersection points and round
// joins use doubles inside Clipper2, which vcpkg compiles with the platform's
// floating-point settings, so outputs agree across platforms except for rare
// half-quantum rounding ties (see docs/geometry.md).

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"

namespace engine::geometry {

inline constexpr double kPolygonScale = 65536.0;          // grid steps per metre
inline constexpr double kPolygonMaxCoordinate = 1.0e6;    // metres

using Ring = std::vector<glm::dvec2>;

struct Polygon {
    Ring outer;              // counter-clockwise
    std::vector<Ring> holes; // clockwise, inside `outer`
};
using PolygonSet = std::vector<Polygon>;

enum class FillRule : std::uint8_t { even_odd, non_zero };
enum class JoinType : std::uint8_t { miter, round, square };
enum class BooleanOp : std::uint8_t { unite, intersect, subtract, exclusive_or };

struct OffsetOptions {
    JoinType join = JoinType::miter;
    double miter_limit = 2.0;      // in multiples of |delta|
    double arc_tolerance = 0.005;  // metres: max chord error of round joins
};

// Signed area (positive for counter-clockwise).
[[nodiscard]] double signed_area(std::span<const glm::dvec2> ring);
[[nodiscard]] double area(const Polygon& polygon);           // outer minus holes
[[nodiscard]] bool contains(const Polygon& polygon, glm::dvec2 point);

// Normalises arbitrary rings (any orientation, self-intersections,
// duplicate points, slivers) into a canonical PolygonSet under `rule`.
// Refuses non-finite or out-of-range coordinates with a precise error.
[[nodiscard]] std::expected<PolygonSet, core::Error> repair(std::span<const Ring> rings,
                                                            FillRule rule = FillRule::non_zero);

// Grows (delta > 0) or shrinks (delta < 0) every polygon. Shrinking may
// split a polygon or remove it.
[[nodiscard]] std::expected<PolygonSet, core::Error> offset(const PolygonSet& polygons, double delta,
                                                            const OffsetOptions& options = {});

[[nodiscard]] std::expected<PolygonSet, core::Error> boolean(BooleanOp op, const PolygonSet& subject,
                                                             const PolygonSet& clip);

// Removes vertices closer than `epsilon` to the line through their
// neighbours (Ramer-Douglas-Peucker style), keeping topology valid.
[[nodiscard]] std::expected<PolygonSet, core::Error> simplify(const PolygonSet& polygons, double epsilon);

} // namespace engine::geometry
