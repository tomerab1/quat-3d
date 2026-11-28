#include "engine/geometry/mesh_ops.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "geometry/det_math.hpp"
#include "geometry/mesh_detail.hpp"

namespace engine::geometry {

namespace {

// Removes triangles with two equal indices, keeping submesh ranges.
void drop_degenerate(MeshData& mesh) {
    const std::vector<asset::SubMesh> subs = submeshes_or_default(mesh);
    std::vector<std::uint32_t> kept;
    kept.reserve(mesh.indices.size());
    std::vector<asset::SubMesh> out_subs;
    for (const asset::SubMesh& s : subs) {
        const auto offset = static_cast<std::uint32_t>(kept.size());
        for (std::uint32_t i = s.index_offset; i + 2 < s.index_offset + s.index_count; i += 3) {
            const std::uint32_t a = mesh.indices[i];
            const std::uint32_t b = mesh.indices[i + 1];
            const std::uint32_t c = mesh.indices[i + 2];
            if (a != b && b != c && a != c) {
                kept.insert(kept.end(), {a, b, c});
            }
        }
        out_subs.push_back(asset::SubMesh{offset, static_cast<std::uint32_t>(kept.size()) - offset, s.material});
    }
    mesh.indices = std::move(kept);
    if (!mesh.submeshes.empty()) {
        mesh.submeshes = std::move(out_subs);
    }
}

using PositionKey = std::array<std::uint32_t, 3>;

PositionKey position_key(glm::vec3 p) {
    return {detail::float_bits(p.x), detail::float_bits(p.y), detail::float_bits(p.z)};
}

} // namespace

void weld(MeshData& mesh, const WeldOptions& options) {
    std::vector<asset::Vertex> corners = detail::corners_of(mesh);
    const float grid = std::max(options.position_tolerance, 0.0F);
    std::map<PositionKey, glm::vec3> snapped; // first position seen per grid cell
    for (asset::Vertex& v : corners) {
        if (grid > 0.0F) {
            const glm::vec3 cell{std::nearbyint(v.position.x / grid), std::nearbyint(v.position.y / grid),
                                 std::nearbyint(v.position.z / grid)};
            v.position = snapped.try_emplace(position_key(cell), v.position).first->second;
        }
    }
    if (options.position_only) {
        std::map<PositionKey, asset::Vertex> first;
        for (asset::Vertex& v : corners) {
            v = first.try_emplace(position_key(v.position), v).first->second;
        }
    }
    detail::rebuild_from_corners(mesh, corners);
    drop_degenerate(mesh);
}

void compute_normals(MeshData& mesh, const NormalOptions& options) {
    const std::size_t triangles = mesh.indices.size() / 3;
    std::vector<glm::dvec3> face_normal(triangles, glm::dvec3{0.0});
    std::vector<std::array<double, 3>> corner_angle(triangles, {0.0, 0.0, 0.0});
    for (std::size_t t = 0; t < triangles; ++t) {
        glm::dvec3 p[3];
        for (int k = 0; k < 3; ++k) {
            p[k] = glm::dvec3(mesh.vertices[mesh.indices[3 * t + k]].position);
        }
        const glm::dvec3 n = glm::cross(p[1] - p[0], p[2] - p[0]);
        const double len = glm::length(n);
        if (len <= 0.0) {
            continue; // degenerate: contributes nothing
        }
        face_normal[t] = n / len;
        for (int k = 0; k < 3; ++k) {
            const glm::dvec3 e1 = p[(k + 1) % 3] - p[k];
            const glm::dvec3 e2 = p[(k + 2) % 3] - p[k];
            const double l1 = glm::length(e1);
            const double l2 = glm::length(e2);
            if (l1 > 0.0 && l2 > 0.0) {
                corner_angle[t][k] = det::acos(glm::dot(e1, e2) / (l1 * l2));
            }
        }
    }
    // Corners per position.
    std::map<PositionKey, std::vector<std::uint32_t>> at_position;
    for (std::uint32_t c = 0; c < mesh.indices.size(); ++c) {
        at_position[position_key(mesh.vertices[mesh.indices[c]].position)].push_back(c);
    }
    const double angle = std::clamp(static_cast<double>(options.crease_angle_degrees), 0.0, 180.0);
    const double cos_crease = det::cos(det::radians(angle));
    std::vector<asset::Vertex> corners = detail::corners_of(mesh);
    for (const auto& [key, list] : at_position) {
        for (const std::uint32_t c : list) {
            const glm::dvec3 own = face_normal[c / 3];
            glm::dvec3 sum{0.0};
            if (angle > 0.0) {
                for (const std::uint32_t o : list) {
                    const glm::dvec3 other = face_normal[o / 3];
                    if (glm::dot(own, other) >= cos_crease - 1e-12) {
                        sum += other * corner_angle[o / 3][o % 3];
                    }
                }
            }
            const double len = glm::length(sum);
            if (len > 0.0) {
                corners[c].normal = glm::vec3(sum / len);
            } else if (glm::dot(own, own) > 0.0) {
                corners[c].normal = glm::vec3(own);
            } // else: a degenerate triangle alone at this position keeps its normal
        }
    }
    detail::rebuild_from_corners(mesh, corners);
}

} // namespace engine::geometry
