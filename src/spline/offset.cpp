// Offset curves in XZ (ROADMAP G18.1): sample, displace along the up-locked
// right vector, trim what comes closer to the curve than the offset distance.

#include <algorithm>
#include <cmath>

#include "engine/spline/frames.hpp"
#include "engine/spline/queries.hpp"

namespace engine::spline {

namespace {

[[nodiscard]] float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

[[nodiscard]] bool segment_hit(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& q0, const glm::vec3& q1,
                               glm::vec3& at) {
    const glm::vec2 a(p0.x, p0.z);
    const glm::vec2 r = glm::vec2(p1.x, p1.z) - a;
    const glm::vec2 c(q0.x, q0.z);
    const glm::vec2 s = glm::vec2(q1.x, q1.z) - c;
    const float den = cross2(r, s);
    if (den == 0.0F) return false;
    const glm::vec2 d = c - a;
    const float t = cross2(d, s) / den;
    const float u = cross2(d, r) / den;
    if (t < 0.0F || t > 1.0F || u < 0.0F || u > 1.0F) return false;
    at = p0 + (p1 - p0) * t;
    return true;
}

// Removes the parts of a raw offset that come closer to the curve than their
// offset distance — the swallowtails inside tight bends and the overlaps of
// a U-turn's inner side — and joins the surviving runs at the crossing of
// the segments that left and re-entered the valid region (or directly).
// This is the distance test of offset trimming (J.-K. Seong, G. Elber,
// M.-S. Kim, "Trimming local and global self-intersections in offset
// curves/surfaces using distance maps", Computer-Aided Design 38(3), 2006),
// with the exact closest-point query in place of a sampled distance map.
void trim_invalid(const Curve& curve, std::vector<glm::vec3>& pts, const std::vector<float>& reach) {
    const std::size_t n = pts.size();
    std::vector<char> ok(n, 1);
    bool any_bad = false;
    bool any_ok = false;
    for (std::size_t i = 0; i < n; ++i) {
        const float want = std::abs(reach[i]);
        const auto c = closest_point(curve, pts[i], true);
        ok[i] = c && c->gap >= want - (1.0e-3F + 0.01F * want) ? 1 : 0;
        any_bad = any_bad || ok[i] == 0;
        any_ok = any_ok || ok[i] != 0;
    }
    if (!any_bad) return;
    if (!any_ok) {
        pts.clear();
        return;
    }
    std::vector<glm::vec3> src = pts;
    if (curve.closed()) {
        // Start the walk on a valid point so no invalid run wraps the seam.
        std::size_t first = 0;
        while (ok[first] == 0) ++first;
        std::rotate(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(first), src.end());
        std::rotate(ok.begin(), ok.begin() + static_cast<std::ptrdiff_t>(first), ok.end());
    }
    pts.clear();
    std::size_t i = 0;
    while (i < n) {
        if (ok[i] != 0) {
            pts.push_back(src[i++]);
            continue;
        }
        std::size_t j = i;
        while (j < n && ok[j] == 0) ++j;
        const bool wraps = j == n && curve.closed();
        const std::size_t b = wraps ? 0 : j;
        if (i > 0 && (j < n || wraps)) {
            glm::vec3 at;
            if (segment_hit(src[i - 1], src[i], src[(b + n - 1) % n], src[b], at)) pts.push_back(at);
        }
        i = j;
    }
}

} // namespace

std::vector<glm::vec3> offset_xz(const Curve& curve, const OffsetOptions& o) {
    std::vector<glm::vec3> out;
    if (curve.empty() || !std::isfinite(o.spacing) || o.spacing <= 0.0F || !std::isfinite(o.distance) ||
        !std::isfinite(o.width_factor)) {
        return out;
    }
    SpacingOptions spacing;
    spacing.spacing = o.spacing;
    spacing.fit_to_ends = true; // open offsets end exactly at the curve's ends
    spacing.max_samples = std::max<std::size_t>(o.max_points, 2);
    const std::vector<CurveSample> samples = sample_by_spacing(curve, spacing);
    out.reserve(samples.size());
    std::vector<float> reach;
    reach.reserve(samples.size());
    for (const CurveSample& s : samples) {
        const Frame f = up_locked_frame(s);
        const float d = o.distance + o.width_factor * 0.5F * s.width;
        reach.push_back(d);
        out.push_back(s.position + f.right * d);
    }
    if (o.trim_loops) trim_invalid(curve, out, reach);
    return out;
}

} // namespace engine::spline
