#include "engine/geometry/mesh_ops.hpp"

#include <cmath>

#include "geometry/mesh_detail.hpp"

namespace engine::geometry {

namespace {

// An orthonormal basis (u, v) of the plane with normal n, chosen without
// trigonometry: u is perpendicular to n and to the world axis n is least
// aligned with.
void plane_basis(glm::dvec3 n, glm::dvec3& u, glm::dvec3& v) {
    const glm::dvec3 a = std::fabs(n.y) < 0.9 ? glm::dvec3{0.0, 1.0, 0.0} : glm::dvec3{1.0, 0.0, 0.0};
    u = glm::normalize(glm::cross(a, n));
    v = glm::cross(n, u);
}

} // namespace

void generate_uvs(MeshData& mesh, const UvOptions& options) {
    const double scale = options.scale > 0.0F ? static_cast<double>(options.scale) : 1.0;
    std::vector<asset::Vertex> corners = detail::corners_of(mesh);
    glm::dvec3 plane_u{1.0, 0.0, 0.0};
    glm::dvec3 plane_v{0.0, 0.0, 1.0};
    if (options.projection == UvProjection::planar) {
        glm::dvec3 n(options.axis);
        n = glm::dot(n, n) > 0.0 ? glm::normalize(n) : glm::dvec3{0.0, 1.0, 0.0};
        plane_basis(n, plane_u, plane_v);
    }
    const glm::dvec3 origin(options.origin);
    for (std::size_t t = 0; t + 2 < corners.size(); t += 3) {
        glm::dvec3 u = plane_u;
        glm::dvec3 v = plane_v;
        if (options.projection == UvProjection::box) {
            const glm::dvec3 a(corners[t].position);
            const glm::dvec3 n = glm::cross(glm::dvec3(corners[t + 1].position) - a,
                                            glm::dvec3(corners[t + 2].position) - a);
            const glm::dvec3 m = glm::abs(n);
            // Dominant axis; ties resolved y, then x, then z (stable).
            if (m.y >= m.x && m.y >= m.z) {
                u = {n.y >= 0.0 ? 1.0 : -1.0, 0.0, 0.0};
                v = {0.0, 0.0, -1.0};
            } else if (m.x >= m.z) {
                u = {0.0, 0.0, n.x >= 0.0 ? -1.0 : 1.0};
                v = {0.0, 1.0, 0.0};
            } else {
                u = {n.z >= 0.0 ? 1.0 : -1.0, 0.0, 0.0};
                v = {0.0, 1.0, 0.0};
            }
        }
        for (std::size_t k = 0; k < 3; ++k) {
            const glm::dvec3 p = glm::dvec3(corners[t + k].position) - origin;
            corners[t + k].uv = glm::vec2(glm::dot(p, u) / scale, glm::dot(p, v) / scale);
        }
    }
    detail::rebuild_from_corners(mesh, corners);
}

} // namespace engine::geometry
