#pragma once

// Curve (ROADMAP G18.1): a compiled spline — its cubic Bézier segments, an
// arc-length table and exact bounds — and evaluation by parameter or by
// distance along the curve.
//
// Parameter t runs over [0, segment_count]: segment i covers [i, i + 1].
// Distance s runs over [0, length()].
//
// Arc length: each segment is split adaptively until a 5-point Gauss–Legendre
// rule and the same rule on both halves agree within a tolerance; the table
// stores the cumulative length at every split. Distance -> parameter inverts
// the table by binary search and then Newton steps on s(u) - s* with a
// bisection fallback inside the table interval (the classic approach, e.g.
// Eberly, "Moving Along a Curve with Specified Speed", Geometric Tools 2008).

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/spline/spline.hpp"

namespace engine::spline {

struct Bounds {
    glm::vec3 min{0.0F};
    glm::vec3 max{0.0F};
    bool operator==(const Bounds&) const = default;
};

// A point on the curve with its interpolated attributes.
struct CurveSample {
    glm::vec3     position{0.0F};
    glm::vec3     tangent{1.0F, 0.0F, 0.0F}; // unit; +X on a degenerate curve
    float         width = 1.0F;
    float         height = 0.0F;
    std::uint32_t tags = 0;
    float         param = 0.0F;    // t
    float         distance = 0.0F; // s
};

struct CurveOptions {
    // Relative tolerance of each table interval's length.
    float arc_tolerance = 1.0e-5F;
    // Table intervals per segment are at most 2^max_depth.
    int   max_depth = 8;
};

class Curve {
public:
    Curve() = default;

    // Validates and compiles `spline`; points are taken as given (apply a
    // transform first with transformed()).
    [[nodiscard]] static std::expected<Curve, core::Error> build(const Spline& spline,
                                                                 const CurveOptions& options = {});
    // From already compiled segments (e.g. a stroke fit).
    [[nodiscard]] static Curve from_segments(std::vector<Segment> segments, bool closed,
                                             const CurveOptions& options = {});

    [[nodiscard]] std::span<const Segment> segments() const { return segments_; }
    [[nodiscard]] std::size_t segment_count() const { return segments_.size(); }
    [[nodiscard]] bool  closed() const { return closed_; }
    [[nodiscard]] bool  empty() const { return segments_.empty(); }
    [[nodiscard]] float length() const { return lengths_.empty() ? 0.0F : lengths_.back(); }
    [[nodiscard]] float max_param() const { return static_cast<float>(segments_.size()); }
    // Tight axis-aligned bounds of the curve itself (Bézier extrema).
    [[nodiscard]] const Bounds& bounds() const { return bounds_; }
    // Bounds grown by half the largest width in XZ and by the largest height
    // upwards: everything a wall / path along the curve can occupy.
    [[nodiscard]] Bounds swept_bounds() const;

    // ---- By parameter -----------------------------------------------------------
    [[nodiscard]] glm::vec3   position(float t) const;
    [[nodiscard]] glm::vec3   derivative(float t) const; // d/dt
    [[nodiscard]] CurveSample sample(float t) const;
    // Arc length from t = 0 to t.
    [[nodiscard]] float distance_at(float t) const;

    // ---- By distance ------------------------------------------------------------
    // Parameter at arc length s (clamped to [0, length]; wrapped when closed).
    [[nodiscard]] float       param_at(float s) const;
    [[nodiscard]] CurveSample sample_at(float s) const;

    // Segment index and local u in [0, 1] of parameter t (clamped).
    void locate(float t, std::size_t& segment, float& u) const;

private:
    struct TableEntry {
        float u = 0.0F; // local parameter of this split in its segment
        float s = 0.0F; // arc length from the segment start
    };

    void build_tables(const CurveOptions& options);
    [[nodiscard]] float segment_distance(std::size_t seg, float u) const;
    [[nodiscard]] float invert(std::size_t seg, float local_s) const;

    std::vector<Segment>    segments_;
    std::vector<float>      lengths_;      // cumulative length at each segment end (size n)
    std::vector<std::uint32_t> table_begin_; // first TableEntry of each segment (size n + 1)
    std::vector<TableEntry> table_;
    Bounds                  bounds_{};
    bool                    closed_ = false;
};

// Exact bounds of one Bézier segment (roots of its derivative per axis).
[[nodiscard]] Bounds segment_bounds(const Segment& segment);

// 5-point Gauss–Legendre arc length of a Bézier over [u0, u1].
[[nodiscard]] float bezier_arc_length(const glm::vec3 (&p)[4], float u0, float u1);

// ---- Even spacing -------------------------------------------------------------------

struct SpacingOptions {
    float spacing = 1.0F;  // metres between samples (> 0)
    float phase = 0.0F;    // distance of the first sample from the start
    // Adjust the spacing so the samples land exactly on both ends (open) or
    // divide the loop evenly (closed): fence posts at the segment ends.
    bool  fit_to_ends = false;
    // Upper bound on the samples returned (untrusted spacing).
    std::size_t max_samples = 1U << 16;
};

// Open: samples at distances phase, phase + spacing, ... up to the length (a
// negative phase continues a pattern that began before the start). Closed:
// one lap of samples from the phase, wrapping past the seam. With
// fit_to_ends the phase is ignored and the samples are at k * length / n.
// Empty on an empty curve or a spacing <= 0.
[[nodiscard]] std::vector<CurveSample> sample_by_spacing(const Curve& curve, const SpacingOptions& options);

// The spline with every point transformed by `m` (an affine map; Bézier and
// polyline curves map exactly, Catmull-Rom up to its knot spacing under
// non-uniform scale). Widths scale with the mean XZ scale.
[[nodiscard]] Spline transformed(const Spline& spline, const glm::mat4& m);

} // namespace engine::spline
