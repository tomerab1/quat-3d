#pragma once

// MeshAsset — a GPU-resident static mesh (Phase 3, Slice 3.2).
//
// One interleaved, device-local vertex buffer plus a uint32 index buffer shared
// across submeshes. Move-only because it owns GpuBuffers; a default-constructed
// MeshAsset is the empty fallback returned for an unloaded handle.

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/rhi/gpu_allocator.hpp"

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

struct MeshAsset {
    rhi::GpuBuffer vertex_buffer;
    rhi::GpuBuffer index_buffer;
    // Skinned meshes only: per-vertex joints/weights (SkinVertex), parallel to the
    // vertex buffer. The skinning compute pass reads vertex_buffer + skin_buffer.
    rhi::GpuBuffer skin_buffer;
    std::uint32_t  vertex_count = 0;
    std::uint32_t  index_count  = 0;
    Aabb           bounds{};
    std::vector<SubMesh> submeshes;

    // Index buffer is always VK_INDEX_TYPE_UINT32 (the loader widens narrower
    // glTF index types on upload).
    static constexpr VkIndexType index_type = VK_INDEX_TYPE_UINT32;

    [[nodiscard]] bool valid() const noexcept {
        return index_count > 0 && index_buffer.handle() != VK_NULL_HANDLE;
    }
    [[nodiscard]] bool skinned() const noexcept {
        return skin_buffer.handle() != VK_NULL_HANDLE;
    }
};

} // namespace engine::asset
