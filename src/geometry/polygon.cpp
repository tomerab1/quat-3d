#include "engine/geometry/polygon.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <utility>

#include <clipper2/clipper.h>

// Clipper2 (Angus Johnson, https://github.com/AngusJohnson/Clipper2; Vatti
// 1992 sweep-line clipping on integer coordinates) does the polygon work.
// This file converts to and from its int64 grid, validates input and makes
// the output canonical, so equal inputs give equal PolygonSets.

namespace engine::geometry {

namespace {

using Clipper2Lib::Path64;
using Clipper2Lib::Paths64;
using Clipper2Lib::Point64;

std::expected<Point64, core::Error> to_grid(glm::dvec2 p, std::size_t ring, std::size_t index) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
        return std::unexpected(core::make_error(
            std::format("polygon ring {} point {} is not finite", ring, index)));
    }
    if (std::fabs(p.x) > kPolygonMaxCoordinate || std::fabs(p.y) > kPolygonMaxCoordinate) {
        return std::unexpected(core::make_error(std::format(
            "polygon ring {} point {} ({}, {}) is outside +-{} m", ring, index, p.x, p.y,
            kPolygonMaxCoordinate)));
    }
    return Point64{static_cast<std::int64_t>(std::nearbyint(p.x * kPolygonScale)),
                   static_cast<std::int64_t>(std::nearbyint(p.y * kPolygonScale))};
}

glm::dvec2 from_grid(const Point64& p) {
    return {static_cast<double>(p.x) / kPolygonScale, static_cast<double>(p.y) / kPolygonScale};
}

std::expected<void, core::Error> append_ring(Paths64& out, std::span<const glm::dvec2> ring,
                                             std::size_t ring_index) {
    Path64 path;
    path.reserve(ring.size());
    for (std::size_t i = 0; i < ring.size(); ++i) {
        auto p = to_grid(ring[i], ring_index, i);
        if (!p) {
            return std::unexpected(p.error());
        }
        path.push_back(*p);
    }
    if (path.size() >= 3) {
        out.push_back(std::move(path));
    }
    return {};
}

std::expected<Paths64, core::Error> to_paths(const PolygonSet& polygons) {
    Paths64 paths;
    std::size_t ring_index = 0;
    for (const Polygon& polygon : polygons) {
        if (auto r = append_ring(paths, polygon.outer, ring_index++); !r) {
            return std::unexpected(r.error());
        }
        for (const Ring& hole : polygon.holes) {
            if (auto r = append_ring(paths, hole, ring_index++); !r) {
                return std::unexpected(r.error());
            }
        }
    }
    return paths;
}

bool point_less(const Point64& a, const Point64& b) {
    return a.x != b.x ? a.x < b.x : a.y < b.y;
}

// Rotates a ring to start at its lexicographically smallest point.
Ring canonical_ring(const Path64& path) {
    const auto first = std::min_element(path.begin(), path.end(), point_less);
    Ring ring;
    ring.reserve(path.size());
    for (auto it = first; it != path.end(); ++it) {
        ring.push_back(from_grid(*it));
    }
    for (auto it = path.begin(); it != first; ++it) {
        ring.push_back(from_grid(*it));
    }
    return ring;
}

bool ring_less(const Ring& a, const Ring& b) {
    // Rings are canonical (start at their minimum), so the first point
    // orders them; equal starts fall back to the full sequence.
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](glm::dvec2 p, glm::dvec2 q) {
                                            return p.x != q.x ? p.x < q.x : p.y < q.y;
                                        });
}

void collect(const Clipper2Lib::PolyPath64& node, PolygonSet& out) {
    // Children of the tree root are outers; their children are holes, whose
    // children are outers again (islands inside holes).
    for (const auto& outer : node) {
        Polygon polygon;
        polygon.outer = canonical_ring(outer->Polygon());
        for (const auto& hole : *outer) {
            polygon.holes.push_back(canonical_ring(hole->Polygon()));
            collect(*hole, out);
        }
        std::sort(polygon.holes.begin(), polygon.holes.end(), ring_less);
        out.push_back(std::move(polygon));
    }
}

// Union under `rule` into a tree, then canonical PolygonSet.
std::expected<PolygonSet, core::Error> canonical(const Paths64& paths, Clipper2Lib::FillRule rule) {
    Clipper2Lib::Clipper64 clipper;
    clipper.PreserveCollinear(false); // canonical rings have no collinear vertices
    clipper.AddSubject(paths);
    Clipper2Lib::PolyTree64 tree;
    if (!clipper.Execute(Clipper2Lib::ClipType::Union, rule, tree)) {
        return std::unexpected(core::make_error("polygon union failed (Clipper2 reported an error)"));
    }
    PolygonSet out;
    collect(tree, out);
    std::sort(out.begin(), out.end(),
              [](const Polygon& a, const Polygon& b) { return ring_less(a.outer, b.outer); });
    return out;
}

Clipper2Lib::FillRule to_clipper(FillRule rule) {
    return rule == FillRule::even_odd ? Clipper2Lib::FillRule::EvenOdd : Clipper2Lib::FillRule::NonZero;
}

} // namespace

double signed_area(std::span<const glm::dvec2> ring) {
    if (ring.size() < 3) {
        return 0.0;
    }
    // Shoelace relative to the first point (better conditioned far from 0).
    double twice = 0.0;
    const glm::dvec2 o = ring[0];
    for (std::size_t i = 1; i + 1 < ring.size(); ++i) {
        const glm::dvec2 a = ring[i] - o;
        const glm::dvec2 b = ring[i + 1] - o;
        twice += a.x * b.y - a.y * b.x;
    }
    return 0.5 * twice;
}

double area(const Polygon& polygon) {
    double a = std::fabs(signed_area(polygon.outer));
    for (const Ring& hole : polygon.holes) {
        a -= std::fabs(signed_area(hole));
    }
    return a;
}

namespace {

bool ring_contains(const Ring& ring, glm::dvec2 p) {
    bool inside = false;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const glm::dvec2 a = ring[i];
        const glm::dvec2 b = ring[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            const double x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (p.x < x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

} // namespace

bool contains(const Polygon& polygon, glm::dvec2 point) {
    if (polygon.outer.size() < 3 || !ring_contains(polygon.outer, point)) {
        return false;
    }
    return std::none_of(polygon.holes.begin(), polygon.holes.end(),
                        [&](const Ring& hole) { return hole.size() >= 3 && ring_contains(hole, point); });
}

std::expected<PolygonSet, core::Error> repair(std::span<const Ring> rings, FillRule rule) {
    Paths64 paths;
    for (std::size_t i = 0; i < rings.size(); ++i) {
        if (auto r = append_ring(paths, rings[i], i); !r) {
            return std::unexpected(r.error());
        }
    }
    return canonical(paths, to_clipper(rule));
}

std::expected<PolygonSet, core::Error> offset(const PolygonSet& polygons, double delta,
                                              const OffsetOptions& options) {
    if (!std::isfinite(delta) || std::fabs(delta) > kPolygonMaxCoordinate) {
        return std::unexpected(core::make_error(std::format("offset delta {} is not a finite distance "
                                                            "within +-{} m", delta, kPolygonMaxCoordinate)));
    }
    if (!(options.miter_limit >= 1.0) || !std::isfinite(options.miter_limit)) {
        return std::unexpected(core::make_error(
            std::format("offset miter_limit {} must be finite and >= 1", options.miter_limit)));
    }
    if (!(options.arc_tolerance > 0.0) || !std::isfinite(options.arc_tolerance)) {
        return std::unexpected(core::make_error(
            std::format("offset arc_tolerance {} must be finite and > 0", options.arc_tolerance)));
    }
    auto paths = to_paths(polygons);
    if (!paths) {
        return std::unexpected(paths.error());
    }
    // Normalise first so orientation (outer CCW, holes CW) is what Clipper2
    // expects: a positive delta grows outers and shrinks holes.
    auto normal = canonical(*paths, Clipper2Lib::FillRule::NonZero);
    if (!normal) {
        return std::unexpected(normal.error());
    }
    auto grid = to_paths(*normal);
    if (!grid) {
        return std::unexpected(grid.error());
    }
    Clipper2Lib::JoinType join = Clipper2Lib::JoinType::Miter;
    switch (options.join) {
    case JoinType::miter: join = Clipper2Lib::JoinType::Miter; break;
    case JoinType::round: join = Clipper2Lib::JoinType::Round; break;
    case JoinType::square: join = Clipper2Lib::JoinType::Square; break;
    }
    Clipper2Lib::ClipperOffset clipper(options.miter_limit, options.arc_tolerance * kPolygonScale);
    clipper.AddPaths(*grid, join, Clipper2Lib::EndType::Polygon);
    Paths64 result;
    clipper.Execute(delta * kPolygonScale, result);
    return canonical(result, Clipper2Lib::FillRule::NonZero);
}

std::expected<PolygonSet, core::Error> boolean(BooleanOp op, const PolygonSet& subject,
                                               const PolygonSet& clip) {
    auto a = to_paths(subject);
    if (!a) {
        return std::unexpected(a.error());
    }
    auto b = to_paths(clip);
    if (!b) {
        return std::unexpected(b.error());
    }
    Clipper2Lib::ClipType type = Clipper2Lib::ClipType::Union;
    switch (op) {
    case BooleanOp::unite: type = Clipper2Lib::ClipType::Union; break;
    case BooleanOp::intersect: type = Clipper2Lib::ClipType::Intersection; break;
    case BooleanOp::subtract: type = Clipper2Lib::ClipType::Difference; break;
    case BooleanOp::exclusive_or: type = Clipper2Lib::ClipType::Xor; break;
    }
    Clipper2Lib::Clipper64 clipper;
    clipper.AddSubject(*a);
    clipper.AddClip(*b);
    Paths64 result;
    if (!clipper.Execute(type, Clipper2Lib::FillRule::NonZero, result)) {
        return std::unexpected(core::make_error("polygon boolean failed (Clipper2 reported an error)"));
    }
    return canonical(result, Clipper2Lib::FillRule::NonZero);
}

std::expected<PolygonSet, core::Error> simplify(const PolygonSet& polygons, double epsilon) {
    if (!(epsilon >= 0.0) || !std::isfinite(epsilon)) {
        return std::unexpected(
            core::make_error(std::format("simplify epsilon {} must be finite and >= 0", epsilon)));
    }
    auto paths = to_paths(polygons);
    if (!paths) {
        return std::unexpected(paths.error());
    }
    const Paths64 simple = Clipper2Lib::SimplifyPaths(*paths, epsilon * kPolygonScale, true);
    // Simplification can make a ring self-touch; the union restores a valid
    // canonical set.
    return canonical(simple, Clipper2Lib::FillRule::NonZero);
}

} // namespace engine::geometry
