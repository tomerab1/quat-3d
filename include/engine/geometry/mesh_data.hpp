#pragma once

// MeshData — a CPU mesh built at runtime (ROADMAP R23.1; design:
// docs/design/cozy-builder.md §4.4). Vertices use the GPU vertex layout
// (asset::Vertex), indices are uint32 triangle lists, and submeshes split
// the index range by material slot. Everything in engine_geometry produces
// or edits MeshData; R23.2's DynamicMeshStore uploads it.
//
// Determinism: every engine_geometry function returns bit-identical MeshData
// for the same inputs on every supported compiler and OS (no FP contraction,
// no libm transcendentals), so mesh_hash() values can be used as cache keys
// and golden test values.

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "engine/asset/mesh_types.hpp"
#include "engine/core/error.hpp"

namespace engine::geometry {

struct MeshData {
    std::vector<asset::Vertex> vertices;
    std::vector<std::uint32_t> indices;   // triangle list, counter-clockwise front faces
    std::vector<asset::SubMesh> submeshes; // index ranges; empty = one submesh, material 0

    [[nodiscard]] std::size_t triangle_count() const noexcept { return indices.size() / 3; }
    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }
};

// Structural checks: index count a multiple of 3, indices in range,
// submeshes contiguous / in range / covering every index, positions and
// attributes finite. Generators run it on their output; DynamicMeshStore
// runs it on what games submit.
[[nodiscard]] std::expected<void, core::Error> validate(const MeshData& mesh);

[[nodiscard]] asset::Aabb compute_bounds(const MeshData& mesh);

// 64-bit FNV-1a over the vertex attributes, indices and submeshes (-0.0 is
// hashed as +0.0). Stable across platforms (little-endian IEEE-754 floats).
[[nodiscard]] std::uint64_t mesh_hash(const MeshData& mesh);

// Appends `src` transformed by `transform` (normals and tangents by its
// inverse transpose, renormalised; winding flipped for mirroring
// transforms). Submeshes keep their materials; an implicit single submesh
// becomes an explicit one.
void append(MeshData& dst, const MeshData& src, const glm::mat4& transform = glm::mat4{1.0F});

// The submesh list with the implicit "one submesh" case made explicit.
[[nodiscard]] std::vector<asset::SubMesh> submeshes_or_default(const MeshData& mesh);

} // namespace engine::geometry
