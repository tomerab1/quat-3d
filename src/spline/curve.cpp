// Curve: compiled spline, arc-length table, bounds and even spacing
// (ROADMAP G18.1). See include/engine/spline/curve.hpp.

#include "engine/spline/curve.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::spline {

namespace {

// 5-point Gauss–Legendre nodes and weights on [-1, 1].
constexpr float k_gl_x[5] = {-0.9061798459386640F, -0.5384693101056831F, 0.0F, 0.5384693101056831F,
                             0.9061798459386640F};
constexpr float k_gl_w[5] = {0.2369268850561891F, 0.4786286704993665F, 0.5688888888888889F, 0.4786286704993665F,
                             0.2369268850561891F};

[[nodiscard]] float speed(const glm::vec3 (&p)[4], float u) {
    const glm::vec3 d = bezier_derivative(p, u);
    return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

[[nodiscard]] glm::vec3 safe_normalize(const glm::vec3& v, const glm::vec3& fallback) {
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (!(len2 > 1.0e-24F)) return fallback;
    return v / std::sqrt(len2);
}

void grow(Bounds& b, const glm::vec3& p) {
    b.min = glm::min(b.min, p);
    b.max = glm::max(b.max, p);
}

// Roots in (0, 1) of a u^2 + b u + c (numerically stable form).
int unit_roots(float a, float b, float c, float (&out)[2]) {
    int n = 0;
    auto keep = [&](float r) {
        if (r > 0.0F && r < 1.0F) out[n++] = r;
    };
    const float scale = std::max({std::abs(a), std::abs(b), std::abs(c)});
    if (scale == 0.0F) return 0;
    if (std::abs(a) <= 1.0e-7F * scale) {
        if (b != 0.0F) keep(-c / b);
        return n;
    }
    const float disc = b * b - 4.0F * a * c;
    if (disc < 0.0F) return 0;
    const float root = std::sqrt(disc);
    const float q = -0.5F * (b + (b < 0.0F ? -root : root));
    if (q != 0.0F) {
        keep(q / a);
        keep(c / q);
    } else {
        keep(0.0F);
    }
    return n;
}

[[nodiscard]] float lerp(float a, float b, float u) { return a + (b - a) * u; }

} // namespace

float bezier_arc_length(const glm::vec3 (&p)[4], float u0, float u1) {
    const float half = 0.5F * (u1 - u0);
    const float mid = 0.5F * (u1 + u0);
    float sum = 0.0F;
    for (int i = 0; i < 5; ++i) sum += k_gl_w[i] * speed(p, mid + half * k_gl_x[i]);
    return sum * half;
}

Bounds segment_bounds(const Segment& s) {
    Bounds b{glm::min(s.p[0], s.p[3]), glm::max(s.p[0], s.p[3])};
    for (int axis = 0; axis < 3; ++axis) {
        const float p0 = s.p[0][axis];
        const float p1 = s.p[1][axis];
        const float p2 = s.p[2][axis];
        const float p3 = s.p[3][axis];
        const float a = -p0 + 3.0F * p1 - 3.0F * p2 + p3;
        const float bb = 2.0F * (p0 - 2.0F * p1 + p2);
        const float c = p1 - p0;
        float roots[2];
        const int n = unit_roots(a, bb, c, roots);
        for (int i = 0; i < n; ++i) grow(b, bezier_point(s.p, roots[i]));
    }
    return b;
}

std::expected<Curve, core::Error> Curve::build(const Spline& spline, const CurveOptions& options) {
    if (auto ok = validate(spline); !ok) return std::unexpected(ok.error());
    return from_segments(build_segments(spline), spline.closed, options);
}

Curve Curve::from_segments(std::vector<Segment> segments, bool closed, const CurveOptions& options) {
    Curve c;
    c.segments_ = std::move(segments);
    c.closed_ = closed;
    c.build_tables(options);
    if (!c.segments_.empty()) {
        c.bounds_ = segment_bounds(c.segments_[0]);
        for (std::size_t i = 1; i < c.segments_.size(); ++i) {
            const Bounds b = segment_bounds(c.segments_[i]);
            grow(c.bounds_, b.min);
            grow(c.bounds_, b.max);
        }
    }
    return c;
}

void Curve::build_tables(const CurveOptions& options) {
    const float tol = std::clamp(options.arc_tolerance, 1.0e-7F, 0.1F);
    const int max_depth = std::clamp(options.max_depth, 1, 16);
    lengths_.clear();
    table_.clear();
    table_begin_.clear();
    float total = 0.0F;
    // Iterative depth-first subdivision; emits interval ends left to right.
    struct Range {
        float u0, u1, whole;
        int   depth;
    };
    std::vector<Range> stack;
    for (const Segment& seg : segments_) {
        table_begin_.push_back(static_cast<std::uint32_t>(table_.size()));
        table_.push_back({0.0F, 0.0F});
        float acc = 0.0F;
        stack.clear();
        stack.push_back({0.0F, 1.0F, bezier_arc_length(seg.p, 0.0F, 1.0F), 0});
        while (!stack.empty()) {
            const Range r = stack.back();
            stack.pop_back();
            const float m = 0.5F * (r.u0 + r.u1);
            const float left = bezier_arc_length(seg.p, r.u0, m);
            const float right = bezier_arc_length(seg.p, m, r.u1);
            const float both = left + right;
            const bool converged = r.depth >= 1 && std::abs(both - r.whole) <= tol * both + 1.0e-9F;
            if (converged || r.depth >= max_depth) {
                acc += left;
                table_.push_back({m, acc});
                acc += right;
                table_.push_back({r.u1, acc});
            } else {
                stack.push_back({m, r.u1, right, r.depth + 1}); // right half after the left
                stack.push_back({r.u0, m, left, r.depth + 1});
            }
        }
        total += acc;
        lengths_.push_back(total);
    }
    table_begin_.push_back(static_cast<std::uint32_t>(table_.size()));
}

void Curve::locate(float t, std::size_t& segment, float& u) const {
    const auto n = segments_.size();
    if (n == 0) {
        segment = 0;
        u = 0.0F;
        return;
    }
    const float tn = static_cast<float>(n);
    if (!std::isfinite(t)) t = 0.0F;
    if (closed_ && (t < 0.0F || t > tn)) {
        t = std::fmod(t, tn);
        if (t < 0.0F) t += tn;
    }
    t = std::clamp(t, 0.0F, tn);
    const auto i = std::min(static_cast<std::size_t>(t), n - 1);
    segment = i;
    u = std::clamp(t - static_cast<float>(i), 0.0F, 1.0F);
}

glm::vec3 Curve::position(float t) const {
    if (segments_.empty()) return glm::vec3(0.0F);
    std::size_t i = 0;
    float u = 0.0F;
    locate(t, i, u);
    return bezier_point(segments_[i].p, u);
}

glm::vec3 Curve::derivative(float t) const {
    if (segments_.empty()) return glm::vec3(0.0F);
    std::size_t i = 0;
    float u = 0.0F;
    locate(t, i, u);
    return bezier_derivative(segments_[i].p, u);
}

CurveSample Curve::sample(float t) const {
    CurveSample out;
    if (segments_.empty()) return out;
    std::size_t i = 0;
    float u = 0.0F;
    locate(t, i, u);
    const Segment& s = segments_[i];
    out.position = bezier_point(s.p, u);
    glm::vec3 d = bezier_derivative(s.p, u);
    if (!(d.x * d.x + d.y * d.y + d.z * d.z > 1.0e-20F)) {
        // A stationary point (coincident control points): the direction of
        // travel is the second derivative's, or failing that the chord's.
        d = bezier_second_derivative(s.p, u);
        if (u >= 0.5F) d = -d;
        if (!(d.x * d.x + d.y * d.y + d.z * d.z > 1.0e-20F)) d = s.p[3] - s.p[0];
    }
    out.tangent = safe_normalize(d, glm::vec3(1.0F, 0.0F, 0.0F));
    out.width = lerp(s.width[0], s.width[1], u);
    out.height = lerp(s.height[0], s.height[1], u);
    out.tags = s.tags;
    out.param = static_cast<float>(i) + u;
    out.distance = (i == 0 ? 0.0F : lengths_[i - 1]) + segment_distance(i, u);
    return out;
}

float Curve::segment_distance(std::size_t seg, float u) const {
    const std::uint32_t b = table_begin_[seg];
    const std::uint32_t e = table_begin_[seg + 1];
    // Last table entry with entry.u <= u.
    std::uint32_t lo = b;
    std::uint32_t hi = e - 1;
    while (lo < hi) {
        const std::uint32_t mid = (lo + hi + 1) / 2;
        if (table_[mid].u <= u) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    const TableEntry& k = table_[lo];
    if (u <= k.u) return k.s;
    return k.s + bezier_arc_length(segments_[seg].p, k.u, u);
}

float Curve::distance_at(float t) const {
    if (segments_.empty()) return 0.0F;
    std::size_t i = 0;
    float u = 0.0F;
    locate(t, i, u);
    return (i == 0 ? 0.0F : lengths_[i - 1]) + segment_distance(i, u);
}

float Curve::invert(std::size_t seg, float ls) const {
    const std::uint32_t b = table_begin_[seg];
    const std::uint32_t e = table_begin_[seg + 1];
    // First interval [k, k + 1] with s[k + 1] >= ls.
    std::uint32_t lo = b;
    std::uint32_t hi = e - 2;
    while (lo < hi) {
        const std::uint32_t mid = (lo + hi) / 2;
        if (table_[mid + 1].s < ls) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    const TableEntry& k0 = table_[lo];
    const TableEntry& k1 = table_[lo + 1];
    const float span_s = k1.s - k0.s;
    if (!(span_s > 0.0F)) return k0.u;
    float a = k0.u;
    float c = k1.u;
    float u = a + (c - a) * std::clamp((ls - k0.s) / span_s, 0.0F, 1.0F);
    const float tol = 1.0e-6F * std::max(span_s, 1.0e-3F);
    const glm::vec3(&p)[4] = segments_[seg].p;
    for (int it = 0; it < 12; ++it) {
        const float f = k0.s + bezier_arc_length(p, k0.u, u) - ls;
        if (std::abs(f) <= tol) break;
        if (f > 0.0F) {
            c = u;
        } else {
            a = u;
        }
        const float v = speed(p, u);
        float next = v > 0.0F ? u - f / v : 0.5F * (a + c);
        if (!(next > a && next < c)) next = 0.5F * (a + c); // Newton left the bracket: bisect
        u = next;
    }
    return u;
}

float Curve::param_at(float s) const {
    if (segments_.empty()) return 0.0F;
    const float total = length();
    if (!std::isfinite(s)) s = 0.0F;
    if (closed_ && total > 0.0F && (s < 0.0F || s >= total)) {
        s = std::fmod(s, total);
        if (s < 0.0F) s += total;
    }
    s = std::clamp(s, 0.0F, total);
    const auto it = std::lower_bound(lengths_.begin(), lengths_.end(), s);
    const auto i = std::min(static_cast<std::size_t>(it - lengths_.begin()), segments_.size() - 1);
    const float start = i == 0 ? 0.0F : lengths_[i - 1];
    return static_cast<float>(i) + invert(i, s - start);
}

CurveSample Curve::sample_at(float s) const {
    CurveSample out = sample(param_at(s));
    const float total = length();
    if (closed_ && total > 0.0F && (s < 0.0F || s >= total)) {
        s = std::fmod(s, total);
        if (s < 0.0F) s += total;
    }
    out.distance = std::isfinite(s) ? std::clamp(s, 0.0F, total) : 0.0F;
    return out;
}

Bounds Curve::swept_bounds() const {
    Bounds b = bounds_;
    float half_width = 0.0F;
    float up = 0.0F;
    float down = 0.0F;
    for (const Segment& s : segments_) {
        for (int k = 0; k < 2; ++k) {
            half_width = std::max(half_width, 0.5F * s.width[k]);
            up = std::max(up, s.height[k]);
            down = std::min(down, s.height[k]);
        }
    }
    b.min -= glm::vec3(half_width, -down, half_width);
    b.max += glm::vec3(half_width, up, half_width);
    return b;
}

std::vector<CurveSample> sample_by_spacing(const Curve& curve, const SpacingOptions& o) {
    std::vector<CurveSample> out;
    const float total = curve.length();
    if (curve.empty() || !std::isfinite(o.spacing) || o.spacing <= 0.0F || !std::isfinite(o.phase)) return out;
    const std::size_t cap = std::max<std::size_t>(o.max_samples, 1);
    if (o.fit_to_ends) {
        const float ratio = total / o.spacing;
        const auto intervals = static_cast<std::size_t>(std::max(1.0F, std::floor(std::min(ratio, 1.0e9F) + 0.5F)));
        const std::size_t count = std::min(curve.closed() ? intervals : intervals + 1, cap);
        out.reserve(count);
        for (std::size_t k = 0; k < count; ++k) {
            const float s = k == intervals ? total
                                           : total * static_cast<float>(k) / static_cast<float>(intervals);
            out.push_back(curve.sample_at(s));
        }
        return out;
    }
    if (curve.closed()) {
        // One lap starting at the phase (wrapped onto the loop).
        for (std::size_t k = 0; k < cap; ++k) {
            const float step = o.spacing * static_cast<float>(k);
            if (step >= total) break;
            out.push_back(curve.sample_at(o.phase + step));
        }
        return out;
    }
    float phase = o.phase;
    if (phase < 0.0F) {
        // A negative phase continues a pattern that started before the curve.
        phase = std::fmod(phase, o.spacing);
        if (phase < 0.0F) phase += o.spacing;
    }
    for (std::size_t k = 0; k < cap; ++k) {
        const float s = phase + o.spacing * static_cast<float>(k);
        if (s > total) break;
        out.push_back(curve.sample_at(s));
    }
    return out;
}

Spline transformed(const Spline& spline, const glm::mat4& m) {
    Spline out = spline;
    const glm::vec3 cx(m[0]);
    const glm::vec3 cy(m[1]);
    const glm::vec3 cz(m[2]);
    const float sx = std::sqrt(cx.x * cx.x + cx.y * cx.y + cx.z * cx.z);
    const float sy = std::sqrt(cy.x * cy.x + cy.y * cy.y + cy.z * cy.z);
    const float sz = std::sqrt(cz.x * cz.x + cz.y * cz.y + cz.z * cz.z);
    const float width_scale = 0.5F * (sx + sz);
    for (SplinePoint& p : out.points) {
        p.position = glm::vec3(m * glm::vec4(p.position, 1.0F));
        p.width *= width_scale;
        p.height *= sy;
    }
    return out;
}

} // namespace engine::spline
