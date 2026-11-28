#pragma once

// MeshAsset — a GPU-resident static mesh (Phase 3, Slice 3.2).
//
// One interleaved, device-local vertex buffer plus a uint32 index buffer shared
// across submeshes. Move-only because it owns GpuBuffers; a default-constructed
// MeshAsset is the empty fallback returned for an unloaded handle.

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/asset/mesh_types.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::asset {

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
