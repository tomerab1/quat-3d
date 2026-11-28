#pragma once

// Polygon triangulation with holes (ROADMAP R23.1), by ear clipping with
// hole bridging: mapbox earcut.hpp (ISC), after Eberly, "Triangulation by
// Ear Clipping" (2002) and Held, "FIST: Fast industrial-strength
// triangulation of polygons" (Algorithmica, 2001).

#include <cstdint>
#include <expected>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/geometry/polygon.hpp"

namespace engine::geometry {

struct Triangulation {
    // The polygon's points: the outer ring, then each hole in order.
    std::vector<glm::dvec2> points;
    // Counter-clockwise triangles indexing `points`.
    std::vector<std::uint32_t> indices;
};

// Triangulates one polygon (outer ring + holes, any orientation). Refuses
// fewer than 3 outer points, non-finite coordinates, and input whose
// triangles do not cover its area (self-intersecting rings: run repair()
// first) with a precise error.
[[nodiscard]] std::expected<Triangulation, core::Error> triangulate(const Polygon& polygon);

} // namespace engine::geometry
