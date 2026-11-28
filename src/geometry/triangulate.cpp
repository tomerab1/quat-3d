#include "engine/geometry/triangulate.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <tuple>

#include <mapbox/earcut.hpp>

namespace mapbox::util {

// earcut reads points through nth<I, T>::get.
template <> struct nth<0, glm::dvec2> {
    static double get(const glm::dvec2& p) { return p.x; }
};
template <> struct nth<1, glm::dvec2> {
    static double get(const glm::dvec2& p) { return p.y; }
};

} // namespace mapbox::util

namespace engine::geometry {

namespace {

double cross(glm::dvec2 a, glm::dvec2 b, glm::dvec2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

} // namespace

std::expected<Triangulation, core::Error> triangulate(const Polygon& polygon) {
    if (polygon.outer.size() < 3) {
        return std::unexpected(core::make_error(
            std::format("triangulate: outer ring has {} points, needs at least 3", polygon.outer.size())));
    }
    std::vector<std::vector<glm::dvec2>> rings;
    rings.reserve(1 + polygon.holes.size());
    rings.push_back(polygon.outer);
    for (const Ring& hole : polygon.holes) {
        if (hole.size() >= 3) {
            rings.push_back(hole);
        }
    }
    Triangulation out;
    for (std::size_t r = 0; r < rings.size(); ++r) {
        for (std::size_t i = 0; i < rings[r].size(); ++i) {
            const glm::dvec2 p = rings[r][i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                return std::unexpected(core::make_error(
                    std::format("triangulate: ring {} point {} is not finite", r, i)));
            }
            out.points.push_back(p);
        }
    }
    if (out.points.size() > std::size_t{UINT32_MAX} / 4) {
        return std::unexpected(core::make_error("triangulate: too many points"));
    }
    out.indices = mapbox::earcut<std::uint32_t>(rings);

    // Make every triangle counter-clockwise and drop zero-area ones (earcut
    // emits them for collinear input points); check coverage.
    std::vector<std::uint32_t> kept;
    kept.reserve(out.indices.size());
    double covered = 0.0;
    for (std::size_t t = 0; t + 2 < out.indices.size(); t += 3) {
        std::array<std::uint32_t, 3> tri{out.indices[t], out.indices[t + 1], out.indices[t + 2]};
        double a = cross(out.points[tri[0]], out.points[tri[1]], out.points[tri[2]]);
        if (a == 0.0) {
            continue;
        }
        if (a < 0.0) {
            std::swap(tri[1], tri[2]);
            a = -a;
        }
        covered += 0.5 * a;
        kept.insert(kept.end(), tri.begin(), tri.end());
    }
    out.indices = std::move(kept);
    const double expected_area = area(Polygon{polygon.outer, {rings.begin() + 1, rings.end()}});
    const double tolerance = 1e-9 * std::max(1.0, std::fabs(expected_area)) + 1e-12;
    if (std::fabs(covered - std::fabs(expected_area)) > std::max(tolerance, 1e-6 * std::fabs(expected_area))) {
        return std::unexpected(core::make_error(std::format(
            "triangulate: triangles cover {} m^2 of a {} m^2 polygon; the rings self-intersect or holes "
            "overlap the outer ring (run geometry::repair first)",
            covered, std::fabs(expected_area))));
    }
    return out;
}

} // namespace engine::geometry
