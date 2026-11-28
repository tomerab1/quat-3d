#pragma once

// SkinningPass — GPU vertex skinning compute pass (Phase 5, Slice 5.4).
//
// Runs skinning.slang once per skinned mesh, before the GBuffer pass, reading the
// bind-pose vertex buffer + per-vertex joints/weights + the per-joint skinning
// matrices and writing a skinned vertex buffer (same interleaved layout as
// asset::Vertex). Skinning writes a storage buffer, not an image, so it is
// recorded directly into the frame command buffer (not the render graph); a
// trailing buffer barrier makes the skinned vertices visible to the GBuffer
// pass's vertex fetch.

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/managed_pipeline.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

// One mesh to skin: storage-buffer device addresses + sizes for the inputs and
// the output, plus the vertex count. The output buffer must be created with
// STORAGE + VERTEX + SHADER_DEVICE_ADDRESS usage.
//
// Velocity (R11): when `prev_positions_out` is set, the vertices are also
// skinned with `prev_joints` (last frame's palette, same joint count; unset =
// `joints`) and their positions written there as one float4 per vertex — the
// DrawItem::prev_positions the GBuffer computes skinned motion from.
struct SkinDispatch {
    VkDeviceAddress vertices_in = 0;
    VkDeviceSize    vertices_in_size = 0;
    VkDeviceAddress skin = 0;
    VkDeviceSize    skin_size = 0;
    VkDeviceAddress joints = 0;
    VkDeviceSize    joints_size = 0;
    VkDeviceAddress vertices_out = 0;
    VkDeviceSize    vertices_out_size = 0;
    std::uint32_t   vertex_count = 0;
    VkDeviceAddress prev_joints = 0;
    VkDeviceAddress prev_positions_out = 0;
    VkDeviceSize    prev_positions_out_size = 0;
};

class SkinningPass {
public:
    [[nodiscard]] static std::expected<SkinningPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    SkinningPass() = default;
    SkinningPass(SkinningPass&&) noexcept = default;
    SkinningPass& operator=(SkinningPass&&) noexcept = default;
    SkinningPass(const SkinningPass&) = delete;
    SkinningPass& operator=(const SkinningPass&) = delete;

    // Record all skinning dispatches into `cmd` followed by one barrier from
    // compute storage-write to vertex-attribute and vertex-shader storage reads
    // (previous positions are read through a device address). The per-dispatch descriptor
    // buffers are owned by the pass and rebuilt each call, so `cmd` must be
    // submitted (and complete) before record() is called again on this instance.
    [[nodiscard]] std::expected<void, core::Error>
    record(VkCommandBuffer cmd, std::span<const SkinDispatch> dispatches);

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           pipeline_;
    std::vector<std::uint32_t>     slots_; // reflected binding of each resource

    std::vector<rhi::DescriptorBuffer> frame_descriptors_;
};

} // namespace engine::renderer
