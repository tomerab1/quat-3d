#include "engine/geometry/mesh_data.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>

namespace engine::geometry {

namespace {

bool finite(glm::vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
bool finite(glm::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(glm::vec4 v) { return finite(glm::vec3(v)) && std::isfinite(v.w); }

struct Fnv {
    std::uint64_t h = 14695981039346656037ULL;
    void byte(std::uint8_t b) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            byte(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void f32(float f) { u32(f == 0.0F ? 0U : std::bit_cast<std::uint32_t>(f)); }
};

} // namespace

std::vector<asset::SubMesh> submeshes_or_default(const MeshData& mesh) {
    if (!mesh.submeshes.empty()) {
        return mesh.submeshes;
    }
    return {asset::SubMesh{0, static_cast<std::uint32_t>(mesh.indices.size()), 0}};
}

std::expected<void, core::Error> validate(const MeshData& mesh) {
    if (mesh.indices.size() % 3 != 0) {
        return std::unexpected(core::make_error(
            std::format("mesh: {} indices is not a whole number of triangles", mesh.indices.size())));
    }
    if (mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
        mesh.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(core::make_error("mesh: more than 2^32 vertices or indices"));
    }
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) {
        if (mesh.indices[i] >= mesh.vertices.size()) {
            return std::unexpected(core::make_error(std::format(
                "mesh: index {} is {}, but there are only {} vertices", i, mesh.indices[i], mesh.vertices.size())));
        }
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const asset::Vertex& v = mesh.vertices[i];
        if (!finite(v.position) || !finite(v.normal) || !finite(v.uv) || !finite(v.tangent)) {
            return std::unexpected(core::make_error(std::format("mesh: vertex {} has a non-finite attribute", i)));
        }
    }
    std::uint32_t expected_offset = 0;
    for (std::size_t s = 0; s < mesh.submeshes.size(); ++s) {
        const asset::SubMesh& sm = mesh.submeshes[s];
        if (sm.index_offset != expected_offset || sm.index_count % 3 != 0) {
            return std::unexpected(core::make_error(std::format(
                "mesh: submesh {} covers indices [{}, +{}); submeshes must be contiguous whole-triangle ranges "
                "starting at {}",
                s, sm.index_offset, sm.index_count, expected_offset)));
        }
        expected_offset += sm.index_count;
    }
    if (!mesh.submeshes.empty() && expected_offset != mesh.indices.size()) {
        return std::unexpected(core::make_error(std::format(
            "mesh: submeshes cover {} of {} indices", expected_offset, mesh.indices.size())));
    }
    return {};
}

asset::Aabb compute_bounds(const MeshData& mesh) {
    if (mesh.vertices.empty()) {
        return {};
    }
    asset::Aabb box{mesh.vertices[0].position, mesh.vertices[0].position};
    for (const asset::Vertex& v : mesh.vertices) {
        box.min = glm::min(box.min, v.position);
        box.max = glm::max(box.max, v.position);
    }
    return box;
}

std::uint64_t mesh_hash(const MeshData& mesh) {
    Fnv h;
    h.u32(static_cast<std::uint32_t>(mesh.vertices.size()));
    for (const asset::Vertex& v : mesh.vertices) {
        for (int i = 0; i < 3; ++i) {
            h.f32(v.position[i]);
        }
        for (int i = 0; i < 3; ++i) {
            h.f32(v.normal[i]);
        }
        h.f32(v.uv.x);
        h.f32(v.uv.y);
        for (int i = 0; i < 4; ++i) {
            h.f32(v.tangent[i]);
        }
    }
    h.u32(static_cast<std::uint32_t>(mesh.indices.size()));
    for (const std::uint32_t i : mesh.indices) {
        h.u32(i);
    }
    for (const asset::SubMesh& s : submeshes_or_default(mesh)) {
        h.u32(s.index_offset);
        h.u32(s.index_count);
        h.u32(s.material);
    }
    return h.h;
}

void append(MeshData& dst, const MeshData& src, const glm::mat4& transform) {
    const glm::mat3 linear(transform);
    const glm::mat3 normal_matrix = glm::transpose(glm::inverse(linear));
    const bool mirrored = glm::determinant(linear) < 0.0F;
    if (dst.submeshes.empty() && !dst.indices.empty()) {
        dst.submeshes = submeshes_or_default(dst);
    }
    const auto base_vertex = static_cast<std::uint32_t>(dst.vertices.size());
    for (asset::Vertex v : src.vertices) {
        v.position = glm::vec3(transform * glm::vec4(v.position, 1.0F));
        const glm::vec3 n = normal_matrix * v.normal;
        v.normal = glm::dot(n, n) > 0.0F ? glm::normalize(n) : n;
        const glm::vec3 t = linear * glm::vec3(v.tangent);
        const float handedness = mirrored ? -v.tangent.w : v.tangent.w;
        v.tangent = glm::vec4(glm::dot(t, t) > 0.0F ? glm::normalize(t) : t, handedness);
        dst.vertices.push_back(v);
    }
    for (const asset::SubMesh& s : submeshes_or_default(src)) {
        const auto offset = static_cast<std::uint32_t>(dst.indices.size());
        for (std::uint32_t i = 0; i + 2 < s.index_count; i += 3) {
            const std::uint32_t a = src.indices[s.index_offset + i] + base_vertex;
            const std::uint32_t b = src.indices[s.index_offset + i + 1] + base_vertex;
            const std::uint32_t c = src.indices[s.index_offset + i + 2] + base_vertex;
            dst.indices.push_back(a);
            dst.indices.push_back(mirrored ? c : b);
            dst.indices.push_back(mirrored ? b : c);
        }
        if (s.index_count > 0) {
            dst.submeshes.push_back(asset::SubMesh{offset, s.index_count, s.material});
        }
    }
}

} // namespace engine::geometry
