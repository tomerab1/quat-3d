#include "engine/geometry/mesh_ops.hpp"

#include <meshoptimizer.h>

// Vertex cache order by meshoptimizer (Kapoulkine; after Forsyth, "Linear-
// speed vertex cache optimisation", 2006), then vertex fetch order. Both
// are deterministic.

namespace engine::geometry {

void optimize(MeshData& mesh) {
    if (mesh.indices.empty() || mesh.vertices.empty()) {
        return;
    }
    std::vector<std::uint32_t> reordered(mesh.indices.size());
    for (const asset::SubMesh& s : submeshes_or_default(mesh)) {
        meshopt_optimizeVertexCache(reordered.data() + s.index_offset, mesh.indices.data() + s.index_offset,
                                    s.index_count, mesh.vertices.size());
    }
    mesh.indices = std::move(reordered);
    std::vector<std::uint32_t> remap(mesh.vertices.size());
    const std::size_t unique = meshopt_optimizeVertexFetchRemap(remap.data(), mesh.indices.data(),
                                                                mesh.indices.size(), mesh.vertices.size());
    std::vector<asset::Vertex> vertices(unique);
    meshopt_remapVertexBuffer(vertices.data(), mesh.vertices.data(), mesh.vertices.size(), sizeof(asset::Vertex),
                              remap.data());
    meshopt_remapIndexBuffer(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), remap.data());
    mesh.vertices = std::move(vertices);
}

} // namespace engine::geometry
