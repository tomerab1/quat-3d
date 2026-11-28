// Closest point on a curve and adaptive flattening (ROADMAP G18.1).

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/spline/queries.hpp"

namespace engine::spline {

namespace {

constexpr int k_coarse_samples = 16;
constexpr int k_newton_steps = 8;
constexpr int k_flatten_depth = 16;
constexpr std::size_t k_flatten_budget = 1U << 20; // chords per flatten() call

[[nodiscard]] float dot3(const glm::vec3& a, const glm::vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

[[nodiscard]] glm::vec3 mask(const glm::vec3& v, bool xz) { return xz ? glm::vec3(v.x, 0.0F, v.z) : v; }

// Squared distance from p to the box (0 inside).
[[nodiscard]] float box_distance2(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& p, bool xz) {
    float d2 = 0.0F;
    for (int a = 0; a < 3; ++a) {
        if (xz && a == 1) continue;
        const float d = p[a] < lo[a] ? lo[a] - p[a] : (p[a] > hi[a] ? p[a] - hi[a] : 0.0F);
        d2 += d * d;
    }
    return d2;
}

struct Candidate {
    float       lower2;
    std::size_t index;
};

// Refines the closest parameter of one segment; returns u and its squared gap.
[[nodiscard]] float refine(const Segment& s, const glm::vec3& p, bool xz, float& out_d2) {
    float best_u = 0.0F;
    float best_d2 = std::numeric_limits<float>::max();
    for (int k = 0; k <= k_coarse_samples; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(k_coarse_samples);
        const glm::vec3 d = mask(bezier_point(s.p, u) - p, xz);
        const float d2 = dot3(d, d);
        if (d2 < best_d2) {
            best_d2 = d2;
            best_u = u;
        }
    }
    float u = best_u;
    for (int it = 0; it < k_newton_steps; ++it) {
        const glm::vec3 diff = mask(bezier_point(s.p, u) - p, xz);
        const glm::vec3 d1 = mask(bezier_derivative(s.p, u), xz);
        const glm::vec3 d2v = mask(bezier_second_derivative(s.p, u), xz);
        const float g = dot3(diff, d1);
        const float gp = dot3(d1, d1) + dot3(diff, d2v);
        if (!(gp > 0.0F)) break; // not a minimum direction: keep the coarse answer
        const float next = std::clamp(u - g / gp, 0.0F, 1.0F);
        const glm::vec3 nd = mask(bezier_point(s.p, next) - p, xz);
        const float nd2 = dot3(nd, nd);
        if (nd2 > best_d2) break; // the step made it worse
        best_d2 = nd2;
        best_u = next;
        if (next == u) break;
        u = next;
    }
    out_d2 = best_d2;
    return best_u;
}

// Max distance of the inner control points from the chord p0 -> p3.
[[nodiscard]] float flatness(const glm::vec3 (&p)[4]) {
    const glm::vec3 chord = p[3] - p[0];
    const float len2 = dot3(chord, chord);
    float worst = 0.0F;
    for (int i = 1; i <= 2; ++i) {
        const glm::vec3 v = p[i] - p[0];
        glm::vec3 off = v;
        if (len2 > 1.0e-20F) off = v - chord * (std::clamp(dot3(v, chord) / len2, 0.0F, 1.0F));
        worst = std::max(worst, dot3(off, off));
    }
    return std::sqrt(worst);
}

// Subdivides `p` (control points relative to `origin`, so flatness and the
// split arithmetic run on small numbers even far from the world origin) until
// it is within `tol` of its chord.
void flatten_segment(const glm::vec3 (&p)[4], const glm::vec3& origin, float u0, float u1, float base, float tol,
                     int depth, int max_depth, std::vector<PolylinePoint>& out) {
    if (depth >= max_depth || flatness(p) <= tol) {
        out.push_back({origin + p[3], base + u1});
        return;
    }
    glm::vec3 l[4];
    glm::vec3 r[4];
    bezier_split(p, 0.5F, l, r);
    const float m = 0.5F * (u0 + u1);
    flatten_segment(l, origin, u0, m, base, tol, depth + 1, max_depth, out);
    flatten_segment(r, origin, m, u1, base, tol, depth + 1, max_depth, out);
}

// The spacing of floats near the curve's largest coordinate: a tolerance
// below a few of these chases rounding noise (and finds phantom crossings).
[[nodiscard]] float coordinate_ulp(const Curve& c) {
    const Bounds& b = c.bounds();
    float m = 0.0F;
    for (int a = 0; a < 3; ++a) m = std::max({m, std::abs(b.min[a]), std::abs(b.max[a])});
    return std::nextafter(m, std::numeric_limits<float>::infinity()) - m;
}

} // namespace

std::optional<ClosestPoint> closest_point(const Curve& curve, const glm::vec3& p, bool xz_only) {
    const auto segs = curve.segments();
    if (segs.empty()) return std::nullopt;
    std::vector<Candidate> order;
    order.reserve(segs.size());
    for (std::size_t i = 0; i < segs.size(); ++i) {
        glm::vec3 lo = segs[i].p[0];
        glm::vec3 hi = segs[i].p[0];
        for (int k = 1; k < 4; ++k) {
            lo = glm::min(lo, segs[i].p[k]);
            hi = glm::max(hi, segs[i].p[k]);
        }
        order.push_back({box_distance2(lo, hi, p, xz_only), i}); // control hull contains the segment
    }
    std::sort(order.begin(), order.end(), [](const Candidate& a, const Candidate& b) {
        return a.lower2 < b.lower2 || (a.lower2 == b.lower2 && a.index < b.index);
    });
    ClosestPoint best;
    float best_d2 = std::numeric_limits<float>::max();
    for (const Candidate& c : order) {
        if (c.lower2 > best_d2) break;
        float d2 = 0.0F;
        const float u = refine(segs[c.index], p, xz_only, d2);
        if (d2 < best_d2) {
            best_d2 = d2;
            best.param = static_cast<float>(c.index) + u;
        }
    }
    best.position = curve.position(best.param);
    best.distance = curve.distance_at(best.param);
    best.gap = std::sqrt(best_d2);
    return best;
}

std::vector<PolylinePoint> flatten(const Curve& curve, float tolerance) {
    std::vector<PolylinePoint> out;
    const auto segs = curve.segments();
    if (segs.empty()) return out;
    float tol = std::isfinite(tolerance) ? std::max(tolerance, 1.0e-6F) : 1.0e-3F;
    tol = std::max(tol, 8.0F * coordinate_ulp(curve));
    // A total chord budget: each segment may split at most 2^depth times.
    int max_depth = 1;
    while (max_depth < k_flatten_depth && (segs.size() << (max_depth + 1)) <= k_flatten_budget) ++max_depth;
    out.reserve(segs.size() + 1);
    out.push_back({segs[0].p[0], 0.0F});
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const glm::vec3 origin = segs[i].p[0];
        const glm::vec3 local[4] = {glm::vec3(0.0F), segs[i].p[1] - origin, segs[i].p[2] - origin,
                                    segs[i].p[3] - origin};
        flatten_segment(local, origin, 0.0F, 1.0F, static_cast<float>(i), tol, 0, max_depth, out);
        out.back().position = segs[i].p[3]; // the segment's exact end, not origin + (end - origin)
    }
    return out;
}

} // namespace engine::spline
