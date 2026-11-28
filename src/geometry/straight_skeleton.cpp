#include "engine/geometry/straight_skeleton.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

#include "geometry/skeleton_wavefront.hpp"

// Algorithm references are in the public header.

namespace engine::geometry {

namespace {

double cross(glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; }

// True when segments ab and cd share a point other than a common endpoint
// (rings may touch at shared vertices: Clipper2's unions produce such weakly
// simple polygons, e.g. a hole pinched against the outer ring).
bool segments_cross(glm::dvec2 a, glm::dvec2 b, glm::dvec2 c, glm::dvec2 d) {
    const double d1 = cross(b - a, c - a);
    const double d2 = cross(b - a, d - a);
    const double d3 = cross(d - c, a - c);
    const double d4 = cross(d - c, b - c);
    if (((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) && ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0))) {
        return true;
    }
    // Touching: an endpoint on the other segment, unless it is that
    // segment's endpoint too.
    auto on = [](glm::dvec2 p, glm::dvec2 q, glm::dvec2 r, double o) {
        return o == 0.0 && r != p && r != q && std::min(p.x, q.x) <= r.x && r.x <= std::max(p.x, q.x) &&
               std::min(p.y, q.y) <= r.y && r.y <= std::max(p.y, q.y);
    };
    if (on(a, b, c, d1) || on(a, b, d, d2) || on(c, d, a, d3) || on(c, d, b, d4)) {
        return true;
    }
    // Collinear overlap with a shared endpoint (one edge folding back on the other).
    if (d1 == 0.0 && d2 == 0.0) {
        const glm::dvec2 dir = b - a;
        auto param = [&](glm::dvec2 p) { return glm::dot(p - a, dir); };
        const double lo = std::max(std::min(param(a), param(b)), std::min(param(c), param(d)));
        const double hi = std::min(std::max(param(a), param(b)), std::max(param(c), param(d)));
        return hi > lo;
    }
    return false;
}

struct CleanRing {
    std::vector<glm::dvec2> points;
    std::vector<double> weights;
    std::vector<std::uint32_t> source; // input edge index per edge
};

// Drops duplicate points and merges collinear edges of equal weight.
CleanRing clean_ring(const Ring& ring, std::span<const double> weights, std::uint32_t first_edge) {
    CleanRing r;
    const std::size_t n = ring.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (ring[i] == ring[(i + 1) % n]) {
            continue; // zero-length edge i
        }
        r.points.push_back(ring[i]);
        r.weights.push_back(weights[i]);
        r.source.push_back(first_edge + static_cast<std::uint32_t>(i));
    }
    bool merged = true;
    while (merged && r.points.size() > 3) {
        merged = false;
        const std::size_t m = r.points.size();
        for (std::size_t i = 0; i < m; ++i) {
            const std::size_t prev = (i + m - 1) % m;
            const glm::dvec2 a = r.points[prev];
            const glm::dvec2 b = r.points[i];
            const glm::dvec2 c = r.points[(i + 1) % m];
            const glm::dvec2 ab = b - a;
            const glm::dvec2 bc = c - b;
            const double scale = glm::length(ab) * glm::length(bc);
            if (std::fabs(cross(ab, bc)) <= 1e-12 * scale && glm::dot(ab, bc) > 0.0 &&
                r.weights[prev] == r.weights[i]) {
                r.points.erase(r.points.begin() + static_cast<std::ptrdiff_t>(i));
                r.weights.erase(r.weights.begin() + static_cast<std::ptrdiff_t>(i));
                r.source.erase(r.source.begin() + static_cast<std::ptrdiff_t>(i));
                merged = true;
                break;
            }
        }
    }
    return r;
}

std::expected<void, core::Error> check_simple(const std::vector<CleanRing>& rings) {
    struct Seg {
        glm::dvec2 a, b;
        std::size_t ring, index;
    };
    std::vector<Seg> segs;
    for (std::size_t r = 0; r < rings.size(); ++r) {
        const auto& p = rings[r].points;
        for (std::size_t i = 0; i < p.size(); ++i) {
            segs.push_back(Seg{p[i], p[(i + 1) % p.size()], r, i});
        }
    }
    for (std::size_t i = 0; i < segs.size(); ++i) {
        for (std::size_t j = i + 1; j < segs.size(); ++j) {
            const Seg& s = segs[i];
            const Seg& t = segs[j];
            if (s.ring == t.ring) {
                const std::size_t n = rings[s.ring].points.size();
                if ((s.index + 1) % n == t.index || (t.index + 1) % n == s.index) {
                    continue; // neighbours share a vertex
                }
            }
            if (segments_cross(s.a, s.b, t.a, t.b)) {
                return std::unexpected(core::make_error(std::format(
                    "straight skeleton: edge {} of ring {} intersects edge {} of ring {}; the polygon must be "
                    "simple (run geometry::repair first)",
                    s.index, s.ring, t.index, t.ring)));
            }
        }
    }
    return {};
}

// Chains the arcs of each face into a counter-clockwise node loop.
std::expected<void, core::Error> build_faces(StraightSkeleton& sk, const std::vector<CleanRing>& rings) {
    const auto edges = static_cast<std::uint32_t>(sk.edge_weights.size());
    std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> half(edges);
    for (const SkeletonArc& arc : sk.arcs) {
        half[arc.left_edge].emplace_back(arc.from, arc.to);
        half[arc.right_edge].emplace_back(arc.to, arc.from);
    }
    std::uint32_t edge = 0;
    for (const CleanRing& ring : rings) {
        const auto n = static_cast<std::uint32_t>(ring.points.size());
        for (std::uint32_t i = 0; i < n; ++i, ++edge) {
            const std::uint32_t a = edge;               // node of the edge's start vertex
            const std::uint32_t b = edge - i + (i + 1) % n; // its end vertex
            std::multimap<std::uint32_t, std::uint32_t> next;
            for (const auto& [from, to] : half[edge]) {
                next.emplace(from, to);
            }
            SkeletonFace face;
            face.edge = edge;
            face.nodes = {a, b};
            std::uint32_t at = b;
            for (std::size_t guard = 0; at != a; ++guard) {
                const auto range = next.equal_range(at);
                if (range.first == range.second || guard > half[edge].size()) {
                    return std::unexpected(core::make_error(std::format(
                        "straight skeleton: face of edge {} is not closed (numerical degeneracy at node {})",
                        edge, at)));
                }
                const auto it = range.first; // smallest target: deterministic
                const std::uint32_t to = it->second;
                next.erase(it);
                if (to != a) {
                    face.nodes.push_back(to);
                }
                at = to;
            }
            sk.faces.push_back(std::move(face));
        }
    }
    return {};
}

} // namespace

std::expected<StraightSkeleton, core::Error> straight_skeleton(const Polygon& polygon,
                                                               std::span<const double> weights) {
    std::vector<const Ring*> input{&polygon.outer};
    for (const Ring& hole : polygon.holes) {
        input.push_back(&hole);
    }
    std::size_t edge_count = 0;
    for (std::size_t r = 0; r < input.size(); ++r) {
        if (input[r]->size() < 3) {
            return std::unexpected(core::make_error(
                std::format("straight skeleton: ring {} has {} points, needs at least 3", r, input[r]->size())));
        }
        for (std::size_t i = 0; i < input[r]->size(); ++i) {
            const glm::dvec2 p = (*input[r])[i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::fabs(p.x) > kPolygonMaxCoordinate ||
                std::fabs(p.y) > kPolygonMaxCoordinate) {
                return std::unexpected(core::make_error(std::format(
                    "straight skeleton: ring {} point {} is not finite or outside +-{} m", r, i,
                    kPolygonMaxCoordinate)));
            }
        }
        edge_count += input[r]->size();
    }
    if (!weights.empty() && weights.size() != edge_count) {
        return std::unexpected(core::make_error(std::format(
            "straight skeleton: {} weights given for {} edges (one per edge, or none)", weights.size(), edge_count)));
    }
    std::vector<double> all_weights(edge_count, 1.0);
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (!std::isfinite(weights[i]) || weights[i] < 0.0 || weights[i] > 1e6) {
            return std::unexpected(core::make_error(
                std::format("straight skeleton: weight {} of edge {} must be finite and in [0, 1e6]", weights[i], i)));
        }
        all_weights[i] = weights[i];
    }

    std::vector<CleanRing> rings;
    std::size_t offset = 0;
    for (std::size_t r = 0; r < input.size(); ++r) {
        const std::span<const double> ring_weights(all_weights.data() + offset, input[r]->size());
        CleanRing ring = clean_ring(*input[r], ring_weights, static_cast<std::uint32_t>(offset));
        offset += input[r]->size();
        if (ring.points.size() < 3) {
            return std::unexpected(core::make_error(
                std::format("straight skeleton: ring {} has no area after removing duplicate points", r)));
        }
        const double a = signed_area(ring.points);
        if (r == 0 ? a <= 0.0 : a >= 0.0) {
            return std::unexpected(core::make_error(std::format(
                "straight skeleton: ring {} must be {} (outer rings counter-clockwise, holes clockwise; "
                "geometry::repair returns that orientation)",
                r, r == 0 ? "counter-clockwise" : "clockwise")));
        }
        if (std::none_of(ring.weights.begin(), ring.weights.end(), [](double w) { return w > 0.0; })) {
            return std::unexpected(
                core::make_error(std::format("straight skeleton: every edge of ring {} has weight 0", r)));
        }
        rings.push_back(std::move(ring));
    }
    if (auto simple = check_simple(rings); !simple) {
        return std::unexpected(simple.error());
    }
    for (std::size_t r = 1; r < rings.size(); ++r) {
        if (!contains(Polygon{rings[0].points, {}}, rings[r].points[0])) {
            return std::unexpected(
                core::make_error(std::format("straight skeleton: hole {} lies outside the outer ring", r - 1)));
        }
    }

    StraightSkeleton out;
    std::vector<std::vector<glm::dvec2>> points;
    std::vector<double> clean_weights;
    for (const CleanRing& ring : rings) {
        points.push_back(ring.points);
        clean_weights.insert(clean_weights.end(), ring.weights.begin(), ring.weights.end());
        out.source_edges.insert(out.source_edges.end(), ring.source.begin(), ring.source.end());
    }
    out.edge_weights = clean_weights;
    skeleton_detail::Wavefront wavefront;
    if (auto r = wavefront.run(points, clean_weights, out); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = build_faces(out, rings); !r) {
        return std::unexpected(r.error());
    }

    // Self-check: the faces of moving edges tile the polygon.
    double polygon_area = 0.0;
    for (const auto& ring : points) {
        polygon_area += signed_area(ring);
    }
    double face_area = 0.0;
    for (const SkeletonFace& face : out.faces) {
        std::vector<glm::dvec2> plan;
        for (const std::uint32_t n : face.nodes) {
            plan.push_back(out.nodes[n].position);
        }
        face_area += signed_area(plan);
    }
    if (std::fabs(face_area - polygon_area) > 1e-6 * polygon_area) {
        return std::unexpected(core::make_error(std::format(
            "straight skeleton: faces cover {} m^2 of a {} m^2 polygon (numerical degeneracy)", face_area,
            polygon_area)));
    }
    return out;
}

} // namespace engine::geometry
