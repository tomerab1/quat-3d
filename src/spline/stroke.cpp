// Whole-stroke fitting and capture (ROADMAP G18.3). See
// include/engine/spline/stroke.hpp for the pipeline; the stages live in
// stroke_fit.cpp.

#include <algorithm>
#include <cmath>
#include <string>

#include "engine/input/actions.hpp"
#include "engine/spline/queries.hpp"
#include "engine/spline/stroke.hpp"

namespace engine::spline {

namespace {

constexpr int k_max_refinements = 12;

[[nodiscard]] float dot3(const glm::vec3& a, const glm::vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] float len3(const glm::vec3& v) { return std::sqrt(dot3(v, v)); }
[[nodiscard]] glm::vec3 unit(const glm::vec3& v) {
    const float l = len3(v);
    return l > 1.0e-12F ? v / l : glm::vec3(0.0F);
}

[[nodiscard]] Segment line_segment(const glm::vec3& a, const glm::vec3& b) {
    Segment s;
    s.p[0] = a;
    s.p[1] = a + (b - a) / 3.0F;
    s.p[2] = b - (b - a) / 3.0F;
    s.p[3] = b;
    return s;
}

// A run of freehand points (resampled, de-jittered) split at its corners.
// `loop`: the run is a whole closed stroke, ending where it starts. Appends
// the pieces to `out` and, per piece, whether its start is a sharp joint
// (a corner, or the joint with what came before) to `sharp`. Returns whether
// a loop's seam is smooth (one tangent across it).
bool fit_freehand(std::span<const glm::vec3> raw, const StrokeOptions& o, bool loop, std::vector<Segment>& out,
                  std::vector<char>& sharp) {
    const std::vector<glm::vec3> dense = resample_polyline(raw, o.sample_spacing);
    const std::vector<std::size_t> keep = simplify_rdp(dense, o.jitter_tolerance);
    std::vector<glm::vec3> clean;
    clean.reserve(keep.size());
    for (const std::size_t i : keep) clean.push_back(dense[i]);
    if (clean.size() < 2) return false;
    // Corners: interior vertices of the simplified polyline that turn sharply.
    std::vector<std::size_t> cuts{0};
    for (std::size_t i = 1; i + 1 < clean.size(); ++i) {
        const glm::vec3 a = unit(clean[i] - clean[i - 1]);
        const glm::vec3 b = unit(clean[i + 1] - clean[i]);
        if (dot3(a, b) < o.corner_cos) cuts.push_back(i);
    }
    cuts.push_back(clean.size() - 1);
    glm::vec3 seam(0.0F);
    if (loop && cuts.size() == 2 && clean.size() >= 3) {
        // A loop without corners (clean.front() == clean.back()): one tangent
        // across the seam, unless the seam itself is a corner.
        const glm::vec3 in = unit(clean[0] - clean[clean.size() - 2]);
        const glm::vec3 out_dir = unit(clean[1] - clean[0]);
        if (dot3(in, out_dir) >= o.corner_cos) seam = unit(clean[1] - clean[clean.size() - 2]);
    }
    for (std::size_t c = 0; c + 1 < cuts.size(); ++c) {
        const std::span<const glm::vec3> piece(clean.data() + cuts[c], cuts[c + 1] - cuts[c] + 1);
        const std::vector<glm::vec3> pts = resample_polyline(piece, o.sample_spacing);
        const glm::vec3 t1 = dot3(seam, seam) > 0.0F ? seam : glm::vec3(0.0F);
        const glm::vec3 t2 = dot3(seam, seam) > 0.0F ? -seam : glm::vec3(0.0F);
        const std::vector<Segment> segs = fit_cubics(pts, o.fit_tolerance, t1, t2);
        for (std::size_t k = 0; k < segs.size(); ++k) sharp.push_back(k == 0 && (c > 0 || !out.empty()) ? 1 : 0);
        out.insert(out.end(), segs.begin(), segs.end());
    }
    return dot3(seam, seam) > 0.0F;
}

// Largest distance between a Catmull-Rom segment and the stretch [ua, ub]
// of a fitted Bézier, both ways (a one-sided check misses overshoot).
[[nodiscard]] float deviation(const Segment& cr, const Segment& bez, float ua, float ub) {
    Segment piece = bez;
    glm::vec3 l[4];
    glm::vec3 r[4];
    if (ub < 1.0F) {
        bezier_split(piece.p, ub, l, r);
        for (int i = 0; i < 4; ++i) piece.p[i] = l[i];
    }
    if (ua > 0.0F) {
        bezier_split(piece.p, ua / ub, l, r);
        for (int i = 0; i < 4; ++i) piece.p[i] = r[i];
    }
    const Curve a = Curve::from_segments({cr}, false);
    const Curve b = Curve::from_segments({piece}, false);
    float worst = 0.0F;
    for (int k = 1; k < 8; ++k) {
        const float u = static_cast<float>(k) / 8.0F;
        worst = std::max(worst, closest_point(a, bezier_point(piece.p, u))->gap);
        worst = std::max(worst, closest_point(b, bezier_point(cr.p, u))->gap);
    }
    return worst;
}

// Centripetal Catmull-Rom points that follow the fitted Béziers within the
// tolerance: start from their anchors and add each over-tolerance span's
// midpoint until none is left (or the point budget is reached). A sharp
// joint (a corner, either end of a straight segment) is a doubled knot: the
// zero knot gap gives chord tangents on both sides (segments.cpp), so the
// corner stays exactly sharp and a straight run between two doubled knots is
// exactly straight.
[[nodiscard]] std::vector<glm::vec3> to_catmull_rom(const std::vector<Segment>& bez, const std::vector<char>& sharp,
                                                    bool closed, const StrokeOptions& o) {
    struct Knot {
        std::size_t bez;
        float       u;
    };
    std::vector<Knot> knots;
    for (std::size_t i = 0; i < bez.size(); ++i) {
        knots.push_back({i, 0.0F});
        if (sharp[i] != 0 && (i > 0 || closed)) knots.push_back({i, 0.0F});
    }
    if (!closed) knots.push_back({bez.size() - 1, 1.0F});
    auto position = [&bez](const Knot& k) { return bezier_point(bez[k.bez].p, k.u); };
    for (int round = 0; round < k_max_refinements; ++round) {
        Spline s;
        s.closed = closed;
        for (const Knot& k : knots) s.points.push_back({position(k), 1.0F, 0.0F, 0});
        const std::vector<Segment> cr = build_segments(s);
        std::vector<Knot> next;
        bool refined = false;
        for (std::size_t j = 0; j < cr.size(); ++j) {
            const Knot a = knots[j];
            const Knot b = knots[(j + 1) % knots.size()];
            next.push_back(a);
            if (b.bez == a.bez && b.u == a.u) continue; // the two halves of a doubled knot
            const float ub = b.bez == a.bez && b.u > a.u ? b.u : 1.0F;
            if (knots.size() + (refined ? 1U : 0U) >= o.max_points) continue;
            if (deviation(cr[j], bez[a.bez], a.u, ub) > o.fit_tolerance) {
                next.push_back({a.bez, 0.5F * (a.u + ub)});
                refined = true;
            }
        }
        if (!closed) next.push_back(knots.back());
        knots = std::move(next);
        if (!refined || knots.size() >= o.max_points) break;
    }
    std::vector<glm::vec3> out;
    out.reserve(knots.size());
    for (const Knot& k : knots) out.push_back(position(k));
    return out;
}

[[nodiscard]] std::expected<void, core::Error> check(const StrokeOptions& o) {
    auto positive = [](float v) { return std::isfinite(v) && v > 0.0F; };
    if (!positive(o.sample_spacing)) return std::unexpected(core::make_error("sample_spacing: expected a number > 0"));
    if (!positive(o.fit_tolerance)) return std::unexpected(core::make_error("fit_tolerance: expected a number > 0"));
    if (!std::isfinite(o.jitter_tolerance) || o.jitter_tolerance < 0.0F) {
        return std::unexpected(core::make_error("jitter_tolerance: expected a number >= 0"));
    }
    if (!std::isfinite(o.corner_cos) || o.corner_cos < -1.0F || o.corner_cos > 1.0F) {
        return std::unexpected(core::make_error("corner_cos: expected a number in [-1, 1]"));
    }
    if (!std::isfinite(o.close_radius) || o.close_radius < 0.0F) {
        return std::unexpected(core::make_error("close_radius: expected a number >= 0"));
    }
    if (o.max_points < 4) return std::unexpected(core::make_error("max_points: expected at least 4"));
    return {};
}

} // namespace

std::expected<StrokeFit, core::Error> fit_stroke(std::span<const StrokePoint> input, const StrokeOptions& o) {
    if (auto ok = check(o); !ok) return std::unexpected(ok.error());
    if (input.size() < 2) {
        return std::unexpected(core::make_error("points: a stroke needs at least 2 points, got " +
                                                std::to_string(input.size())));
    }
    float length = 0.0F;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const glm::vec3& p = input[i].position;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            std::abs(p.x) > k_max_coordinate || std::abs(p.y) > k_max_coordinate || std::abs(p.z) > k_max_coordinate) {
            return std::unexpected(core::make_error("points[" + std::to_string(i) +
                                                    "]: expected finite coordinates within +-1e6 m"));
        }
        if (i > 0) length += len3(p - input[i - 1].position);
    }
    if (length / o.sample_spacing > static_cast<float>(k_max_resampled)) {
        return std::unexpected(core::make_error(
            "sample_spacing: use >= " + std::to_string(length / static_cast<float>(k_max_resampled)) +
            " m for this " + std::to_string(length) + " m stroke (at most " + std::to_string(k_max_resampled) +
            " resampled points)"));
    }
    if (!(length >= o.min_length)) {
        return std::unexpected(core::make_error("the stroke is " + std::to_string(length) +
                                                " m long, shorter than min_length " + std::to_string(o.min_length)));
    }
    // Loop closing: the end came back near the start.
    std::vector<StrokePoint> pts(input.begin(), input.end());
    const glm::vec3 start = pts.front().position;
    auto xz_gap = [&start](const glm::vec3& p) {
        const float dx = p.x - start.x;
        const float dz = p.z - start.z;
        return std::sqrt(dx * dx + dz * dz);
    };
    const bool closed = o.close_radius > 0.0F && length >= 4.0F * o.close_radius &&
                        xz_gap(pts.back().position) <= o.close_radius && pts.size() >= 3;
    if (closed) {
        // Drop the freehand tail inside the radius; the loop ends at the start.
        while (pts.size() > 3 && !pts.back().straight && xz_gap(pts[pts.size() - 2].position) <= o.close_radius) {
            pts.pop_back();
        }
        pts.back() = {start, pts.back().straight};
    }
    // Pieces: runs of freehand points, and single straight segments.
    std::vector<Segment> bez;
    std::vector<char> sharp; // per piece: its start is a sharp joint
    bool smooth_seam = false;
    const bool single_freehand =
        std::none_of(pts.begin() + 1, pts.end(), [](const StrokePoint& p) { return p.straight; });
    std::vector<glm::vec3> run{pts[0].position};
    auto flush = [&](bool loop) {
        if (run.size() >= 2) smooth_seam = fit_freehand(run, o, loop, bez, sharp);
        run.clear();
    };
    for (std::size_t i = 1; i < pts.size(); ++i) {
        if (pts[i].straight) {
            flush(false);
            sharp.push_back(bez.empty() ? 0 : 1);
            bez.push_back(line_segment(pts[i - 1].position, pts[i].position));
            run.push_back(pts[i].position);
        } else {
            run.push_back(pts[i].position);
        }
    }
    flush(closed && single_freehand);
    if (bez.empty()) return std::unexpected(core::make_error("the stroke has no extent after resampling"));
    if (closed) {
        bez.back().p[3] = bez.front().p[0]; // exact seam
        sharp[0] = smooth_seam ? 0 : 1;
    }

    StrokeFit fit;
    fit.closed = closed;
    fit.spline.closed = closed;
    fit.spline.follow_ground = false;
    if (o.output == StrokeOutput::cubic_bezier) {
        fit.spline.kind = SplineKind::cubic_bezier;
        for (const Segment& s : bez) {
            for (int k = 0; k < 3; ++k) fit.spline.points.push_back({s.p[k], o.width, 0.0F, 0});
        }
        if (!closed) fit.spline.points.push_back({bez.back().p[3], o.width, 0.0F, 0});
    } else {
        fit.spline.kind = SplineKind::centripetal_catmull_rom;
        for (const glm::vec3& p : to_catmull_rom(bez, sharp, closed, o)) fit.spline.points.push_back({p, o.width, 0.0F, 0});
    }
    if (fit.spline.points.size() > o.max_points) {
        return std::unexpected(core::make_error("the fit needs " + std::to_string(fit.spline.points.size()) +
                                                " points, more than max_points " + std::to_string(o.max_points) +
                                                " (raise fit_tolerance)"));
    }
    fit.beziers = std::move(bez);
    if (auto ok = validate(fit.spline); !ok) return std::unexpected(ok.error());
    return fit;
}

// ---- StrokeCapture ---------------------------------------------------------------

void StrokeCapture::begin(const glm::vec3& hit) {
    points_.clear();
    points_.push_back({hit, false});
    active_ = true;
    straight_ = false;
}

void StrokeCapture::move(const glm::vec3& hit, bool straight) {
    if (!active_) return;
    if (straight) {
        if (!straight_) {
            straight_ = true;
            points_.push_back({hit, true}); // the straight segment starts at the last point
        } else {
            points_.back().position = hit;
        }
        return;
    }
    straight_ = false;
    if (len3(hit - points_.back().position) < 0.2F * options_.sample_spacing) return;
    points_.push_back({hit, false});
}

std::expected<StrokeFit, core::Error> StrokeCapture::end() {
    active_ = false;
    straight_ = false;
    return fit_stroke(points_, options_);
}

void StrokeCapture::cancel() {
    active_ = false;
    straight_ = false;
    points_.clear();
}

StrokeEvent StrokeCapture::update(const input::Actions& actions, const StrokeBindings& bindings,
                                  const PointerGroundFn& ground) {
    const input::ActionState* draw = actions.state(bindings.draw);
    if (draw == nullptr || !ground) return StrokeEvent::none;
    const input::ActionState* mod = bindings.straight.empty() ? nullptr : actions.state(bindings.straight);
    const bool straight = mod != nullptr && mod->active;
    StrokeEvent event = StrokeEvent::none;
    if (!active_ && draw->started) {
        const std::optional<glm::vec3> origin = ground(draw->drag_origin);
        const std::optional<glm::vec3> here = ground(draw->pointer);
        if (!origin && !here) return StrokeEvent::none;
        begin(origin ? *origin : *here);
        if (here) move(*here, straight);
        event = StrokeEvent::began;
    } else if (active_ && draw->active) {
        if (const std::optional<glm::vec3> here = ground(draw->pointer)) move(*here, straight);
        event = StrokeEvent::moved;
    }
    if (active_ && (draw->ended || !draw->active)) {
        result_ = end();
        event = result_->has_value() ? StrokeEvent::finished : StrokeEvent::failed;
    }
    return event;
}

std::optional<std::expected<StrokeFit, core::Error>> StrokeCapture::take_result() {
    auto out = std::move(result_);
    result_.reset();
    return out;
}

} // namespace engine::spline
