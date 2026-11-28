#pragma once

// CPU-side mesh layout types shared by the GPU mesh asset (mesh_asset.hpp),
// the glTF loader and runtime-generated geometry (engine_geometry, ROADMAP
// R23). Graphics-API free, so it is part of the SDK header set.

#include <cstdint>

#include <glm/glm.hpp>

namespace engine::asset {

// Interleaved vertex layout for static meshes. Mirrors the GBuffer vertex
// shader input that arrives in Phase 3.5.
struct Vertex {
    glm::vec3 position{0.0F};
    glm::vec3 normal{0.0F, 0.0F, 1.0F};
    glm::vec2 uv{0.0F};
    glm::vec4 tangent{1.0F, 0.0F, 0.0F, 1.0F}; // xyz = tangent, w = bitangent sign
};

// Per-vertex skinning influences (parallel to the vertex array), for skinned
// meshes only. `joints` indexes into the skeleton's joint array; `weights` sum
// to 1. Matches the SkinVertex layout the skinning compute shader reads.
struct SkinVertex {
    glm::uvec4 joints{0U};
    glm::vec4  weights{0.0F};
};

// Axis-aligned bounding box in mesh-local space.
struct Aabb {
    glm::vec3 min{0.0F};
    glm::vec3 max{0.0F};
};

// One renderable primitive: a contiguous range into the shared index buffer and
// (resolved in Phase 3.4) a material slot. A glTF mesh can hold several.
struct SubMesh {
    std::uint32_t index_offset = 0;
    std::uint32_t index_count  = 0;
    std::uint32_t material     = 0;
};

} // namespace engine::asset
