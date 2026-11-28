#pragma once

// Splines (ROADMAP G18.1): the authored data of a curve — control points with
// per-point width / height / tags — and its compilation into cubic Bézier
// segments, the one form every query in engine/spline/ works on.
//
// Kinds:
//   centripetal_catmull_rom  passes through every point; no cusps or
//                            self-intersections within a segment (Yuksel,
//                            Schaefer & Keyser 2011). The default, for
//                            player-drawn curves.
//   cubic_bezier             points are anchor, handle, handle, anchor, ...
//                            (3k + 1 points open, 3k closed: the last two
//                            handles lead back to point 0). For authored curves.
//   polyline                 straight segments between the points.
//
// Everything here is CPU-only, Vulkan-free and deterministic: only + - * /
// and sqrt (correctly rounded under IEEE 754), evaluated in a fixed order and
// compiled without floating-point contraction (CMakeLists.txt), so the same
// spline gives bit-identical results on every platform.

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"

namespace engine::spline {

struct SplinePoint {
    glm::vec3     position{0.0F};
    float         width = 1.0F;  // metres across the curve (walls, paths, rivers)
    float         height = 0.0F; // metres above the curve (wall height, bank)
    std::uint32_t tags = 0;      // game-defined bits (gate here, no fence posts, ...)
    bool operator==(const SplinePoint&) const = default;
};

enum class SplineKind : std::uint8_t { centripetal_catmull_rom, cubic_bezier, polyline };

// The `spline` component (entity-local points). Plain data: the derived
// curve lives in SplineCache (engine/scene/spline.hpp) or a Curve built from it.
struct Spline {
    SplineKind               kind = SplineKind::centripetal_catmull_rom;
    std::vector<SplinePoint> points;
    bool                     closed = false;
    // Sampled positions take their y from the ground (GroundHeightFn) plus
    // ground_offset; points keep their authored y.
    bool                     follow_ground = true;
    float                    ground_offset = 0.0F;
    // Curve network (G18.2) this spline joins; 0 = none. Splines sharing a
    // network are snapped, split at crossings and classified together.
    std::uint32_t            network = 0;
    // Stable id within the network, from which node / edge ids derive
    // (0 = let the owner pick one, e.g. the entity).
    std::uint32_t            id = 0;
    bool operator==(const Spline&) const = default;
};

// Hard limits for untrusted input (codec, remote, Lua).
inline constexpr std::size_t k_max_points = 16384;
inline constexpr float       k_max_coordinate = 1.0e6F;
inline constexpr float       k_max_width = 1.0e4F;

// Name <-> kind ("centripetal_catmull_rom", "cubic_bezier", "polyline").
[[nodiscard]] std::string_view kind_name(SplineKind kind);
[[nodiscard]] bool             parse_kind(std::string_view name, SplineKind& out);

// A cubic Bézier piece of a compiled spline, with the attributes of the two
// spline points it joins (interpolated linearly in u).
struct Segment {
    glm::vec3     p[4]{};           // control points
    float         width[2]{1.0F, 1.0F};
    float         height[2]{0.0F, 0.0F};
    std::uint32_t tags = 0;          // tags of the start point
    std::uint32_t first_point = 0;   // spline point at u = 0
};

// Why a spline cannot be compiled: too few points, a Bézier point count that
// is not 3k + 1 (3k closed), non-finite or out-of-range values, too many points.
[[nodiscard]] std::expected<void, core::Error> validate(const Spline& spline);

// Number of segments a valid spline compiles to.
[[nodiscard]] std::size_t segment_count(const Spline& spline);

// Compiles a spline into cubic Bézier segments (validate() first; an invalid
// spline gives an empty list). Catmull-Rom segments use centripetal knots
// (alpha = 1/2: knot gaps are sqrt(|P[i+1] - P[i]|)); an open curve's end
// tangents come from reflected phantom points (P[-1] = 2 P[0] - P[1]).
// Coincident neighbours never divide by zero: a zero knot gap gives a
// straight (linear) tangent instead.
[[nodiscard]] std::vector<Segment> build_segments(const Spline& spline);

// ---- Cubic Bézier evaluation (de Casteljau / Bernstein form) ----------------

[[nodiscard]] glm::vec3 bezier_point(const glm::vec3 (&p)[4], float u);
[[nodiscard]] glm::vec3 bezier_derivative(const glm::vec3 (&p)[4], float u);
[[nodiscard]] glm::vec3 bezier_second_derivative(const glm::vec3 (&p)[4], float u);
// Splits at u into [0, u] (left) and [u, 1] (right).
void bezier_split(const glm::vec3 (&p)[4], float u, glm::vec3 (&left)[4], glm::vec3 (&right)[4]);

// Bit-stable 64-bit hash of a spline's content (FNV-1a over its fields), for
// caches that rebuild only when the spline changes.
[[nodiscard]] std::uint64_t content_hash(const Spline& spline, std::uint64_t seed = 0);

} // namespace engine::spline
