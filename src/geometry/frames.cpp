#include <algorithm>
#include <cmath>
#include <format>

#include "engine/geometry/sweep.hpp"
#include "geometry/det_math.hpp"

namespace engine::geometry {

namespace {

// Rotates v about the unit axis by `angle` (Rodrigues).
glm::dvec3 rotate(glm::dvec3 v, glm::dvec3 axis, double angle) {
    double s = 0.0;
    double c = 0.0;
    det::sincos(angle, s, c);
    return v * c + glm::cross(axis, v) * s + axis * (glm::dot(axis, v) * (1.0 - c));
}

} // namespace

std::expected<std::vector<PathFrame>, core::Error> path_frames(std::span<const glm::dvec3> path,
                                                               const FrameOptions& options) {
    std::vector<glm::dvec3> points(path.begin(), path.end());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const glm::dvec3 p = points[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            return std::unexpected(core::make_error(std::format("path point {} is not finite", i)));
        }
    }
    if (options.closed && points.size() > 1 && points.front() == points.back()) {
        points.pop_back(); // a closed path may repeat its start
    }
    const std::size_t n = points.size();
    if (n < 2 || (options.closed && n < 3)) {
        return std::unexpected(core::make_error(
            std::format("path has {} distinct points; needs {}", n, options.closed ? 3 : 2)));
    }
    const std::size_t segments = options.closed ? n : n - 1;
    std::vector<glm::dvec3> seg(segments);
    std::vector<double> seg_len(segments);
    for (std::size_t k = 0; k < segments; ++k) {
        const glm::dvec3 d = points[(k + 1) % n] - points[k];
        seg_len[k] = glm::length(d);
        if (!(seg_len[k] > 0.0)) {
            return std::unexpected(core::make_error(std::format("path points {} and {} coincide", k, (k + 1) % n)));
        }
        seg[k] = d / seg_len[k];
    }
    // cos(179 degrees): sharper turns fold the swept surface onto itself.
    constexpr double u_turn = -0.9998476951563913;
    std::vector<PathFrame> frames(n);
    double distance = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const bool has_in = options.closed || i > 0;
        const bool has_out = options.closed || i + 1 < n;
        const glm::dvec3 in = has_in ? seg[(i + segments - 1) % segments] : seg[0];
        const glm::dvec3 out = has_out ? seg[i % segments] : in;
        const double c = glm::dot(in, out);
        if (c < u_turn) {
            return std::unexpected(core::make_error(std::format(
                "path turns back on itself at point {} (more than 179 degrees); round or split the path", i)));
        }
        PathFrame& f = frames[i];
        f.origin = points[i];
        f.tangent = glm::normalize(in + out);
        f.distance = distance;
        if (has_out) {
            distance += seg_len[i % segments];
        }
        // cos(half turn) = dot(in, bisector).
        f.miter_scale = std::max(1.0, 1.0 / glm::dot(in, f.tangent)); // >= 1 despite rounding
        const glm::dvec3 turn = out - in;
        const double turn_len = glm::length(turn);
        f.miter_direction = turn_len > 1e-12 ? -(turn / turn_len) : glm::dvec3{0.0};
    }
    // First frame: binormal leans towards `up`.
    glm::dvec3 up = options.up - frames[0].tangent * glm::dot(options.up, frames[0].tangent);
    if (glm::length(up) < 1e-9) {
        const glm::dvec3 alt = std::fabs(frames[0].tangent.x) < 0.9 ? glm::dvec3{1, 0, 0} : glm::dvec3{0, 0, 1};
        up = alt - frames[0].tangent * glm::dot(alt, frames[0].tangent);
    }
    frames[0].binormal = glm::normalize(up);
    frames[0].normal = glm::cross(frames[0].binormal, frames[0].tangent);
    // Double reflection (Wang et al. 2008, Algorithm 1).
    auto transport = [&](const PathFrame& a, glm::dvec3 next_origin, glm::dvec3 next_tangent) {
        const glm::dvec3 v1 = next_origin - a.origin;
        const double c1 = glm::dot(v1, v1);
        const glm::dvec3 r_l = a.normal - v1 * (2.0 / c1 * glm::dot(v1, a.normal));
        const glm::dvec3 t_l = a.tangent - v1 * (2.0 / c1 * glm::dot(v1, a.tangent));
        const glm::dvec3 v2 = next_tangent - t_l;
        const double c2 = glm::dot(v2, v2);
        glm::dvec3 r = c2 > 0.0 ? r_l - v2 * (2.0 / c2 * glm::dot(v2, r_l)) : r_l;
        // Re-orthonormalise against rounding drift over long paths.
        r = glm::normalize(r - next_tangent * glm::dot(r, next_tangent));
        return r;
    };
    for (std::size_t i = 1; i < n; ++i) {
        frames[i].normal = transport(frames[i - 1], frames[i].origin, frames[i].tangent);
        frames[i].binormal = glm::cross(frames[i].tangent, frames[i].normal);
    }
    if (options.closed) {
        // Holonomy: the frame transported once around differs from the first
        // by a rotation about its tangent; spread it along the length.
        const glm::dvec3 back = transport(frames[n - 1], frames[0].origin, frames[0].tangent);
        const double angle = det::atan2(glm::dot(glm::cross(back, frames[0].normal), frames[0].tangent),
                                        glm::dot(back, frames[0].normal));
        const double total = distance;
        for (std::size_t i = 1; i < n; ++i) {
            PathFrame& f = frames[i];
            f.normal = glm::normalize(rotate(f.normal, f.tangent, angle * (f.distance / total)));
            f.binormal = glm::cross(f.tangent, f.normal);
        }
    }
    return frames;
}

} // namespace engine::geometry
