// Stroke fitting stages (ROADMAP G18.3): resampling, Ramer–Douglas–Peucker
// simplification and Schneider's piecewise cubic fit.
//
// U. Ramer, "An iterative procedure for the polygonal approximation of plane
// curves", Computer Graphics and Image Processing 1(3), 1972; D. Douglas,
// T. Peucker, "Algorithms for the reduction of the number of points required
// to represent a digitized line or its caricature", The Canadian
// Cartographer 10(2), 1973.
//
// P. J. Schneider, "An Algorithm for Automatically Fitting Digitized Curves",
// Graphics Gems (A. Glassner, ed.), Academic Press, 1990: chord-length
// parameters, a least-squares Bézier with fixed end tangents, Newton–Raphson
// reparameterisation, and a split at the worst point with a shared tangent.

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/spline/stroke.hpp"

namespace engine::spline {

namespace {

constexpr int k_max_reparameterise = 8;
constexpr int k_max_depth = 24;

[[nodiscard]] float dot3(const glm::vec3& a, const glm::vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] float len3(const glm::vec3& v) { return std::sqrt(dot3(v, v)); }

[[nodiscard]] glm::vec3 unit(const glm::vec3& v) {
    const float l = len3(v);
    return l > 1.0e-12F ? v / l : glm::vec3(0.0F);
}

// Squared distance from p to segment a-b.
[[nodiscard]] float segment_distance2(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float l2 = dot3(ab, ab);
    const float t = l2 > 0.0F ? std::clamp(dot3(p - a, ab) / l2, 0.0F, 1.0F) : 0.0F;
    const glm::vec3 d = p - (a + ab * t);
    return dot3(d, d);
}

class Fitter {
public:
    Fitter(std::span<const glm::vec3> d, float tolerance) : d_(d), tol2_(tolerance * tolerance) {}

    void fit(std::size_t first, std::size_t last, glm::vec3 t1, glm::vec3 t2, int depth) {
        const std::size_t n = last - first + 1;
        if (n == 2 || depth >= k_max_depth) {
            const float dist = len3(d_[last] - d_[first]) / 3.0F;
            emit({d_[first], d_[first] + t1 * dist, d_[last] + t2 * dist, d_[last]});
            return;
        }
        std::vector<float> u = chord_parameters(first, last);
        Bez bez = generate(first, last, u, t1, t2);
        std::size_t split = 0;
        float err = max_error(first, last, bez, u, split);
        if (err <= tol2_) {
            emit(bez);
            return;
        }
        // Newton steps on the parameters before splitting. Schneider tries them
        // only below 4x the squared tolerance; trying them up to 100x the
        // tolerance gives fewer pieces for the same accuracy.
        if (err <= 1.0e4F * tol2_) {
            for (int i = 0; i < k_max_reparameterise; ++i) {
                reparameterise(first, last, u, bez);
                bez = generate(first, last, u, t1, t2);
                err = max_error(first, last, bez, u, split);
                if (err <= tol2_) {
                    emit(bez);
                    return;
                }
            }
        }
        // Split at the worst point with the tangent across it.
        split = std::clamp(split, first + 1, last - 1);
        glm::vec3 center = unit(d_[split - 1] - d_[split + 1]);
        if (dot3(center, center) == 0.0F) center = unit(d_[split - 1] - d_[split]);
        fit(first, split, t1, center, depth + 1);
        fit(split, last, -center, t2, depth + 1);
    }

    std::vector<Segment> take() { return std::move(out_); }

private:
    struct Bez {
        glm::vec3 p[4];
    };

    void emit(const Bez& b) {
        Segment s;
        for (int i = 0; i < 4; ++i) s.p[i] = b.p[i];
        out_.push_back(s);
    }

    [[nodiscard]] std::vector<float> chord_parameters(std::size_t first, std::size_t last) const {
        std::vector<float> u(last - first + 1, 0.0F);
        for (std::size_t i = first + 1; i <= last; ++i) {
            u[i - first] = u[i - first - 1] + len3(d_[i] - d_[i - 1]);
        }
        const float total = u.back();
        for (float& x : u) x = total > 0.0F ? x / total : 0.0F;
        u.back() = 1.0F;
        return u;
    }

    // Least-squares inner control points along the fixed end tangents.
    [[nodiscard]] Bez generate(std::size_t first, std::size_t last, const std::vector<float>& u,
                               const glm::vec3& t1, const glm::vec3& t2) const {
        float c00 = 0.0F;
        float c01 = 0.0F;
        float c11 = 0.0F;
        float x0 = 0.0F;
        float x1 = 0.0F;
        const glm::vec3 p0 = d_[first];
        const glm::vec3 p3 = d_[last];
        for (std::size_t i = first; i <= last; ++i) {
            const float t = u[i - first];
            const float v = 1.0F - t;
            const float b0 = v * v * v;
            const float b1 = 3.0F * t * v * v;
            const float b2 = 3.0F * t * t * v;
            const float b3 = t * t * t;
            const glm::vec3 a1 = t1 * b1;
            const glm::vec3 a2 = t2 * b2;
            c00 += dot3(a1, a1);
            c01 += dot3(a1, a2);
            c11 += dot3(a2, a2);
            const glm::vec3 tmp = d_[i] - (p0 * (b0 + b1) + p3 * (b2 + b3));
            x0 += dot3(a1, tmp);
            x1 += dot3(a2, tmp);
        }
        const float det = c00 * c11 - c01 * c01;
        float alpha_l = 0.0F;
        float alpha_r = 0.0F;
        if (det != 0.0F) {
            alpha_l = (x0 * c11 - x1 * c01) / det;
            alpha_r = (c00 * x1 - c01 * x0) / det;
        }
        const float seg = len3(p3 - p0);
        const float eps = 1.0e-6F * seg;
        if (!(alpha_l >= eps) || !(alpha_r >= eps)) {
            // Degenerate least squares: Wu / Barsky heuristic, a third of the chord.
            alpha_l = seg / 3.0F;
            alpha_r = alpha_l;
        }
        return {{p0, p0 + t1 * alpha_l, p3 + t2 * alpha_r, p3}};
    }

    [[nodiscard]] float max_error(std::size_t first, std::size_t last, const Bez& b, const std::vector<float>& u,
                                  std::size_t& split) const {
        float worst = 0.0F;
        split = (first + last) / 2;
        for (std::size_t i = first + 1; i < last; ++i) {
            const glm::vec3 d = bezier_point(b.p, u[i - first]) - d_[i];
            const float e = dot3(d, d);
            if (e >= worst) {
                worst = e;
                split = i;
            }
        }
        return worst;
    }

    void reparameterise(std::size_t first, std::size_t last, std::vector<float>& u, const Bez& b) const {
        for (std::size_t i = first; i <= last; ++i) {
            float& t = u[i - first];
            const glm::vec3 q = bezier_point(b.p, t) - d_[i];
            const glm::vec3 q1 = bezier_derivative(b.p, t);
            const glm::vec3 q2 = bezier_second_derivative(b.p, t);
            const float num = dot3(q, q1);
            const float den = dot3(q1, q1) + dot3(q, q2);
            if (den != 0.0F) t = std::clamp(t - num / den, 0.0F, 1.0F);
        }
    }

    std::span<const glm::vec3> d_;
    float                      tol2_;
    std::vector<Segment>       out_;
};

} // namespace

std::vector<glm::vec3> resample_polyline(std::span<const glm::vec3> pts, float spacing) {
    std::vector<glm::vec3> out;
    if (pts.size() < 2 || !(spacing > 0.0F) || !std::isfinite(spacing)) {
        out.assign(pts.begin(), pts.end());
        return out;
    }
    out.push_back(pts[0]);
    float carry = 0.0F; // distance walked since the last emitted point
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const glm::vec3 a = pts[i - 1];
        const glm::vec3 b = pts[i];
        const float seg = len3(b - a);
        if (!(seg > 0.0F)) continue;
        float s = spacing - carry; // first emission on this segment
        while (s <= seg) {
            out.push_back(a + (b - a) * (s / seg));
            s += spacing;
        }
        carry = seg - (s - spacing);
    }
    // The last point exactly (replacing a sample that landed very close).
    if (len3(out.back() - pts.back()) < 0.5F * spacing && out.size() > 1) {
        out.back() = pts.back();
    } else if (out.back() != pts.back()) {
        out.push_back(pts.back());
    }
    return out;
}

std::vector<std::size_t> simplify_rdp(std::span<const glm::vec3> pts, float tolerance) {
    std::vector<std::size_t> keep;
    if (pts.empty()) return keep;
    if (pts.size() < 3) {
        for (std::size_t i = 0; i < pts.size(); ++i) keep.push_back(i);
        return keep;
    }
    const float tol2 = std::max(tolerance, 0.0F) * std::max(tolerance, 0.0F);
    std::vector<char> kept(pts.size(), 0);
    kept.front() = 1;
    kept.back() = 1;
    std::vector<std::pair<std::size_t, std::size_t>> stack{{0, pts.size() - 1}};
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        float worst = -1.0F;
        std::size_t at = a;
        for (std::size_t i = a + 1; i < b; ++i) {
            const float d2 = segment_distance2(pts[i], pts[a], pts[b]);
            if (d2 > worst) {
                worst = d2;
                at = i;
            }
        }
        if (at != a && worst > tol2) {
            kept[at] = 1;
            stack.push_back({at, b});
            stack.push_back({a, at});
        }
    }
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (kept[i] != 0) keep.push_back(i);
    }
    return keep;
}

std::vector<Segment> fit_cubics(std::span<const glm::vec3> pts, float tolerance, glm::vec3 t1, glm::vec3 t2) {
    if (pts.size() < 2) return {};
    if (dot3(t1, t1) == 0.0F) t1 = unit(pts[1] - pts[0]);
    if (dot3(t2, t2) == 0.0F) t2 = unit(pts[pts.size() - 2] - pts.back());
    t1 = unit(t1);
    t2 = unit(t2);
    Fitter f(pts, std::max(tolerance, 1.0e-6F));
    f.fit(0, pts.size() - 1, t1, t2, 0);
    return f.take();
}

} // namespace engine::spline
