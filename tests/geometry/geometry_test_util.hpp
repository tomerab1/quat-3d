#pragma once

// Helpers for the engine_geometry tests.

#include <cmath>
#include <random>

#include <glm/glm.hpp>

#include "engine/geometry/mesh_data.hpp"
#include "engine/geometry/polygon.hpp"

namespace quat::test::geometry {

using engine::geometry::MeshData;
using engine::geometry::Polygon;
using engine::geometry::Ring;

inline double triangle_area(const MeshData& m, std::size_t t) {
    const glm::dvec3 a(m.vertices[m.indices[3 * t]].position);
    const glm::dvec3 b(m.vertices[m.indices[3 * t + 1]].position);
    const glm::dvec3 c(m.vertices[m.indices[3 * t + 2]].position);
    return 0.5 * glm::length(glm::cross(b - a, c - a));
}

inline double surface_area(const MeshData& m) {
    double a = 0.0;
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        a += triangle_area(m, t);
    }
    return a;
}

inline glm::dvec3 triangle_normal(const MeshData& m, std::size_t t) {
    const glm::dvec3 a(m.vertices[m.indices[3 * t]].position);
    const glm::dvec3 b(m.vertices[m.indices[3 * t + 1]].position);
    const glm::dvec3 c(m.vertices[m.indices[3 * t + 2]].position);
    const glm::dvec3 n = glm::cross(b - a, c - a);
    const double l = glm::length(n);
    return l > 0.0 ? n / l : n;
}

// Signed volume of a closed mesh (divergence theorem); positive when the
// triangles face outward.
inline double signed_volume(const MeshData& m) {
    double v = 0.0;
    for (std::size_t t = 0; t < m.triangle_count(); ++t) {
        const glm::dvec3 a(m.vertices[m.indices[3 * t]].position);
        const glm::dvec3 b(m.vertices[m.indices[3 * t + 1]].position);
        const glm::dvec3 c(m.vertices[m.indices[3 * t + 2]].position);
        v += glm::dot(a, glm::cross(b, c)) / 6.0;
    }
    return v;
}

// A star-shaped random polygon (counter-clockwise, simple).
inline Polygon random_star(std::mt19937& rng, int n, double r0 = 1.0, double r1 = 5.0) {
    std::uniform_real_distribution<double> radius(r0, r1);
    Polygon p;
    for (int i = 0; i < n; ++i) {
        const double a = 6.283185307179586 * i / n;
        const double r = radius(rng);
        p.outer.push_back({r * std::cos(a), r * std::sin(a)});
    }
    return p;
}

inline Ring rect(double x, double y, double w, double h) { return {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}; }

} // namespace quat::test::geometry
