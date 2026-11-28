// XZ crossings of curves (ROADMAP G18.1): flatten, intersect chords through a
// bounding-volume hierarchy, refine on the curves by Newton steps.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "engine/spline/queries.hpp"

namespace engine::spline {

namespace {

struct Chord {
    glm::vec2 a{0.0F};
    glm::vec2 b{0.0F};
    float     ta = 0.0F; // curve params at the ends
    float     tb = 0.0F;
    std::uint32_t index = 0; // position in the flattened polyline
};

struct Node {
    glm::vec2     lo{0.0F};
    glm::vec2     hi{0.0F};
    std::uint32_t first = 0; // leaf: chords [first, first + count); inner: left child = first
    std::uint32_t count = 0; // 0 = inner node (right child = left + 1 is not assumed; see right)
    std::uint32_t right = 0;
};

constexpr std::uint32_t k_leaf_size = 4;

[[nodiscard]] glm::vec2 xz(const glm::vec3& v) { return {v.x, v.z}; }

[[nodiscard]] float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

class ChordTree {
public:
    explicit ChordTree(std::vector<Chord> chords) : chords_(std::move(chords)) {
        if (!chords_.empty()) build(0, static_cast<std::uint32_t>(chords_.size()));
    }

    template <typename Fn>
    void query(const glm::vec2& lo, const glm::vec2& hi, Fn&& fn) const {
        if (nodes_.empty()) return;
        std::uint32_t stack[64];
        int top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const Node& n = nodes_[stack[--top]];
            if (n.hi.x < lo.x || n.lo.x > hi.x || n.hi.y < lo.y || n.lo.y > hi.y) continue;
            if (n.count > 0) {
                for (std::uint32_t i = n.first; i < n.first + n.count; ++i) fn(chords_[i]);
            } else if (top + 2 <= 64) {
                stack[top++] = n.right;
                stack[top++] = n.first;
            }
        }
    }

private:
    std::uint32_t build(std::uint32_t first, std::uint32_t count) {
        const auto id = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        glm::vec2 lo = glm::min(chords_[first].a, chords_[first].b);
        glm::vec2 hi = glm::max(chords_[first].a, chords_[first].b);
        for (std::uint32_t i = first + 1; i < first + count; ++i) {
            lo = glm::min(lo, glm::min(chords_[i].a, chords_[i].b));
            hi = glm::max(hi, glm::max(chords_[i].a, chords_[i].b));
        }
        nodes_[id].lo = lo;
        nodes_[id].hi = hi;
        if (count <= k_leaf_size) {
            nodes_[id].first = first;
            nodes_[id].count = count;
            return id;
        }
        // Median split along the wider axis by chord midpoint (ties by index:
        // a total order, so the tree is the same everywhere).
        const int axis = (hi.x - lo.x) >= (hi.y - lo.y) ? 0 : 1;
        const auto mid = first + count / 2;
        std::nth_element(chords_.begin() + first, chords_.begin() + mid, chords_.begin() + first + count,
                         [axis](const Chord& p, const Chord& q) {
                             const float cp = p.a[axis] + p.b[axis];
                             const float cq = q.a[axis] + q.b[axis];
                             return cp < cq || (cp == cq && p.index < q.index);
                         });
        const std::uint32_t left = build(first, mid - first);
        const std::uint32_t right = build(mid, first + count - mid);
        nodes_[id].first = left;
        nodes_[id].count = 0;
        nodes_[id].right = right;
        return id;
    }

    std::vector<Chord> chords_;
    std::vector<Node>  nodes_;
};

[[nodiscard]] std::vector<Chord> chords_of(const Curve& c, float tol) {
    const std::vector<PolylinePoint> pts = flatten(c, tol);
    std::vector<Chord> out;
    if (pts.size() < 2) return out;
    out.reserve(pts.size() - 1);
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        out.push_back({xz(pts[i].position), xz(pts[i + 1].position), pts[i].param, pts[i + 1].param,
                       static_cast<std::uint32_t>(i)});
    }
    return out;
}

// Chord / chord intersection: parameters along each in [0, 1].
[[nodiscard]] bool chord_hit(const Chord& p, const Chord& q, float& sp, float& sq) {
    const glm::vec2 r = p.b - p.a;
    const glm::vec2 s = q.b - q.a;
    const float den = cross2(r, s);
    if (std::abs(den) <= 1.0e-12F * (std::abs(r.x) + std::abs(r.y)) * (std::abs(s.x) + std::abs(s.y)) ||
        den == 0.0F) {
        return false; // parallel (collinear overlaps are not crossings)
    }
    const glm::vec2 d = q.a - p.a;
    sp = cross2(d, s) / den;
    sq = cross2(d, r) / den;
    constexpr float e = 1.0e-5F;
    return sp >= -e && sp <= 1.0F + e && sq >= -e && sq <= 1.0F + e;
}

// Newton on A(ta) - B(tb) = 0 in XZ, kept inside the chords' parameter spans.
void refine(const Curve& ca, const Curve& cb, float lo_a, float hi_a, float lo_b, float hi_b, float& ta,
            float& tb) {
    for (int it = 0; it < 8; ++it) {
        const glm::vec2 f = xz(ca.position(ta)) - xz(cb.position(tb));
        if (f.x * f.x + f.y * f.y <= 1.0e-14F) break;
        const glm::vec2 da = xz(ca.derivative(ta));
        const glm::vec2 db = -xz(cb.derivative(tb));
        const float det = cross2(da, db);
        if (std::abs(det) <= 1.0e-12F) break;
        // [da db] [dx dy]^T = -f
        const float dx = -cross2(f, db) / det;
        const float dy = -cross2(da, f) / det;
        const float na = std::clamp(ta + dx, lo_a, hi_a);
        const float nb = std::clamp(tb + dy, lo_b, hi_b);
        if (na == ta && nb == tb) break;
        ta = na;
        tb = nb;
    }
}

[[nodiscard]] float slack(const Chord& c) { return 0.5F * (c.tb - c.ta); }

// Distance along `c` between two parameters (the shorter way round a loop).
[[nodiscard]] float along(const Curve& c, float t0, float t1) {
    const float d = std::abs(c.distance_at(t0) - c.distance_at(t1));
    return c.closed() ? std::min(d, c.length() - d) : d;
}

void finish(const Curve& a, const Curve& b, std::vector<Crossing>& hits, float merge, bool self) {
    if (self) {
        // One crossing per unordered pair; a loop's end parameter is its start.
        for (Crossing& h : hits) {
            if (a.closed() && h.param_a >= a.max_param()) h.param_a = 0.0F;
            if (a.closed() && h.param_b >= a.max_param()) h.param_b = 0.0F;
            if (h.param_b < h.param_a) {
                std::swap(h.param_a, h.param_b);
                std::swap(h.position_a, h.position_b);
            }
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Crossing& p, const Crossing& q) {
        return p.param_a < q.param_a || (p.param_a == q.param_a && p.param_b < q.param_b);
    });
    std::vector<Crossing> out;
    for (const Crossing& h : hits) {
        bool dup = false;
        for (const Crossing& o : out) {
            const glm::vec2 d = xz(o.position_a) - xz(h.position_a);
            if (d.x * d.x + d.y * d.y > merge * merge) continue;
            const bool same = along(a, o.param_a, h.param_a) <= merge * 4.0F &&
                              along(b, o.param_b, h.param_b) <= merge * 4.0F;
            const bool swapped = self && along(a, o.param_a, h.param_b) <= merge * 4.0F &&
                                 along(a, o.param_b, h.param_a) <= merge * 4.0F;
            if (same || swapped) {
                dup = true;
                break;
            }
        }
        if (!dup) out.push_back(h);
    }
    if (self) {
        // A "crossing" of a parameter with itself (a loop's seam) is none.
        std::erase_if(out, [&](const Crossing& h) { return along(a, h.param_a, h.param_b) <= merge * 4.0F; });
    }
    hits = std::move(out);
}

[[nodiscard]] std::vector<Crossing> crossings_impl(const Curve& a, const Curve& b, const CrossingOptions& o,
                                                   bool self) {
    std::vector<Crossing> hits;
    if (a.empty() || b.empty()) return hits;
    const float tol = std::isfinite(o.tolerance) ? std::max(o.tolerance, 1.0e-6F) : 1.0e-3F;
    const float merge = std::isfinite(o.merge_distance) ? std::max(o.merge_distance, 0.0F) : 1.0e-3F;
    const std::vector<Chord> ca = chords_of(a, tol);
    std::vector<Chord> cb_list = chords_of(b, tol);
    const auto last_b = static_cast<std::uint32_t>(cb_list.size()) - 1;
    const ChordTree tree(std::move(cb_list));
    for (const Chord& p : ca) {
        const glm::vec2 lo = glm::min(p.a, p.b);
        const glm::vec2 hi = glm::max(p.a, p.b);
        tree.query(lo, hi, [&](const Chord& q) {
            if (self) {
                if (q.index <= p.index + 1) return; // each pair once; neighbours share a point
                if (b.closed() && p.index == 0 && q.index == last_b) return; // the seam
            }
            float sp = 0.0F;
            float sq = 0.0F;
            if (!chord_hit(p, q, sp, sq)) return;
            float ta = p.ta + (p.tb - p.ta) * std::clamp(sp, 0.0F, 1.0F);
            float tb = q.ta + (q.tb - q.ta) * std::clamp(sq, 0.0F, 1.0F);
            refine(a, b, std::max(p.ta - slack(p), 0.0F), std::min(p.tb + slack(p), a.max_param()),
                   std::max(q.ta - slack(q), 0.0F), std::min(q.tb + slack(q), b.max_param()), ta, tb);
            if (self && std::abs(tb - ta) < 1.0e-4F) return;
            hits.push_back({ta, tb, a.position(ta), b.position(tb)});
        });
    }
    finish(a, b, hits, merge, self);
    return hits;
}

} // namespace

std::vector<Crossing> crossings_xz(const Curve& a, const Curve& b, const CrossingOptions& options) {
    return crossings_impl(a, b, options, false);
}

std::vector<Crossing> self_crossings_xz(const Curve& curve, const CrossingOptions& options) {
    return crossings_impl(curve, curve, options, true);
}

} // namespace engine::spline
