#pragma once

// Helpers shared by the MeshData operations (private to engine_geometry).

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "engine/geometry/mesh_data.hpp"

namespace engine::geometry::detail {

// A vertex's bits (-0 folded into +0), for exact deduplication.
using VertexKey = std::array<std::uint32_t, 12>;

inline std::uint32_t float_bits(float f) { return f == 0.0F ? 0U : std::bit_cast<std::uint32_t>(f); }

inline VertexKey vertex_key(const asset::Vertex& v) {
    return {float_bits(v.position.x), float_bits(v.position.y), float_bits(v.position.z),
            float_bits(v.normal.x),   float_bits(v.normal.y),   float_bits(v.normal.z),
            float_bits(v.uv.x),       float_bits(v.uv.y),       float_bits(v.tangent.x),
            float_bits(v.tangent.y),  float_bits(v.tangent.z),  float_bits(v.tangent.w)};
}

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& k) const noexcept {
        std::uint64_t h = 14695981039346656037ULL;
        for (const std::uint32_t w : k) {
            h = (h ^ w) * 1099511628211ULL;
        }
        return static_cast<std::size_t>(h);
    }
};

// Replaces the vertex buffer by one vertex per index (`corners[i]` is the
// vertex of index i), merging identical vertices; new vertices are numbered
// in first-use order, so the result is deterministic.
inline void rebuild_from_corners(MeshData& mesh, std::span<const asset::Vertex> corners) {
    std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> index_of;
    index_of.reserve(corners.size());
    std::vector<asset::Vertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const auto [it, inserted] =
            index_of.try_emplace(vertex_key(corners[i]), static_cast<std::uint32_t>(vertices.size()));
        if (inserted) {
            vertices.push_back(corners[i]);
        }
        mesh.indices[i] = it->second;
    }
    mesh.vertices = std::move(vertices);
}

// The vertex at every index.
inline std::vector<asset::Vertex> corners_of(const MeshData& mesh) {
    std::vector<asset::Vertex> corners;
    corners.reserve(mesh.indices.size());
    for (const std::uint32_t i : mesh.indices) {
        corners.push_back(mesh.vertices[i]);
    }
    return corners;
}

} // namespace engine::geometry::detail
