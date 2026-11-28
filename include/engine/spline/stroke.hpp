#pragma once

// Stroke capture (ROADMAP G18.3): a pointer drag over the ground becomes a
// clean spline. Shared by games (drawing walls, paths, fences) and the
// editor's spline tool.
//
// Pipeline (fit_stroke):
//   ground hits -> resample by distance -> Ramer–Douglas–Peucker (removes
//   hand jitter, finds corners) -> Schneider's least-squares piecewise cubic
//   fit within a tolerance (P. J. Schneider, "An Algorithm for Automatically
//   Fitting Digitized Curves", Graphics Gems, 1990) -> centripetal
//   Catmull-Rom points (refined until the curve stays within the tolerance
//   of the fit) or the Bézier segments themselves.
// Options close a loop when the stroke ends near its start, and keep
// segments drawn with the straight modifier (Shift) exactly straight.

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/spline/spline.hpp"

namespace engine::input {
class Actions;
}

namespace engine::spline {

// ---- Pipeline stages (exposed for reuse and tests) -------------------------

// Points every `spacing` metres along the polyline (3D arc length), first and
// last included. Fewer than two input points are returned as they are.
[[nodiscard]] std::vector<glm::vec3> resample_polyline(std::span<const glm::vec3> points, float spacing);

// Ramer–Douglas–Peucker (U. Ramer 1972; D. Douglas & T. Peucker 1973):
// indices of the points kept so that no dropped point is farther than
// `tolerance` from the simplified polyline. First and last are always kept.
[[nodiscard]] std::vector<std::size_t> simplify_rdp(std::span<const glm::vec3> points, float tolerance);

// Schneider's fit of one piece between corners: cubic Béziers through the
// first and last point whose distance to every point is at most
// `tolerance`. `tangent_start` points into the piece and `tangent_end` back
// into it from the last point (zero: estimated from the points).
[[nodiscard]] std::vector<Segment> fit_cubics(std::span<const glm::vec3> points, float tolerance,
                                              glm::vec3 tangent_start = glm::vec3(0.0F),
                                              glm::vec3 tangent_end = glm::vec3(0.0F));

// ---- Whole strokes -----------------------------------------------------------

enum class StrokeOutput : std::uint8_t { catmull_rom, cubic_bezier };

struct StrokeOptions {
    float        sample_spacing = 0.25F;  // resampling distance (metres)
    float        jitter_tolerance = 0.05F; // RDP tolerance (metres)
    float        fit_tolerance = 0.1F;     // max distance of the curve from the stroke (metres)
    // A turn sharper than this (cosine of the angle between the incoming and
    // outgoing direction; 0.5 = 60 degrees) is a corner, kept sharp.
    float        corner_cos = 0.5F;
    // Close the loop when the stroke ends within this distance of its start
    // (and is at least 4x as long). 0 never closes.
    float        close_radius = 0.75F;
    float        min_length = 0.25F;      // shorter strokes are refused
    StrokeOutput output = StrokeOutput::catmull_rom;
    float        width = 1.0F;            // per-point width of the result
    std::size_t  max_points = 4096;       // of the result
};

// A captured point. `straight` marks the segment from the previous point to
// this one as drawn straight (the straight modifier was held).
struct StrokePoint {
    glm::vec3 position{0.0F};
    bool      straight = false;
};

struct StrokeFit {
    Spline               spline;   // kind per StrokeOptions::output; follow_ground false
    std::vector<Segment> beziers;  // the fitted cubic pieces
    bool                 closed = false;
};

// Fits a whole stroke. Errors: fewer than 2 points, non-finite points, a
// stroke shorter than min_length, invalid options.
[[nodiscard]] std::expected<StrokeFit, core::Error> fit_stroke(std::span<const StrokePoint> points,
                                                               const StrokeOptions& options = {});

// ---- Capture -------------------------------------------------------------------

// The ground point under a viewport pixel (a raycast against terrain and
// colliders), nullopt when the pointer is over nothing.
using PointerGroundFn = std::function<std::optional<glm::vec3>(glm::vec2 pointer_px)>;

// Names of the G5 input actions that drive capture: `draw`, a button action
// (a `drag` trigger keeps clicks from drawing), and `straight`, a button
// action held for straight segments (empty: none).
struct StrokeBindings {
    std::string draw = "stroke.draw";
    std::string straight = "stroke.straight";
};

enum class StrokeEvent : std::uint8_t { none, began, moved, finished, failed, cancelled };

class StrokeCapture {
public:
    explicit StrokeCapture(StrokeOptions options = {}) : options_(options) {}

    void begin(const glm::vec3& hit);
    // A new ground hit. Freehand hits closer than a fifth of the sample
    // spacing to the last point are skipped. While `straight` is held, the
    // segment from the point where it was pressed follows the pointer.
    void move(const glm::vec3& hit, bool straight);
    // Ends the stroke and fits it.
    [[nodiscard]] std::expected<StrokeFit, core::Error> end();
    void cancel();

    [[nodiscard]] bool active() const { return active_; }
    // The raw stroke so far (for a preview line).
    [[nodiscard]] std::span<const StrokePoint> points() const { return points_; }
    [[nodiscard]] const StrokeOptions& options() const { return options_; }
    void set_options(const StrokeOptions& options) { options_ = options; }

    // Drives capture from input actions (G5.1): the draw action starting
    // begins at the ground under its press point, staying active adds the
    // ground under the pointer, ending fits (finished / failed; take the
    // result with take_result()). Missing ground hits are skipped.
    StrokeEvent update(const input::Actions& actions, const StrokeBindings& bindings,
                       const PointerGroundFn& ground);
    // The last update()'s fit (finished or failed), once.
    [[nodiscard]] std::optional<std::expected<StrokeFit, core::Error>> take_result();

private:
    StrokeOptions                                         options_;
    std::vector<StrokePoint>                              points_;
    bool                                                  active_ = false;
    bool                                                  straight_ = false;
    std::optional<std::expected<StrokeFit, core::Error>> result_;
};

} // namespace engine::spline
