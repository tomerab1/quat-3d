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
#include "engine/rhi/compute_pipeline.hpp"
#include "engine/rhi/descriptor_buffer.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer {

// One mesh to skin: storage-buffer device addresses + sizes for the inputs and
// the output, plus the vertex count. The output buffer must be created with
// STORAGE + VERTEX + SHADER_DEVICE_ADDRESS usage.
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
    // compute storage-write to vertex-attribute read. The per-dispatch descriptor
    // buffers are owned by the pass and rebuilt each call, so `cmd` must be
    // submitted (and complete) before record() is called again on this instance.
    [[nodiscard]] std::expected<void, core::Error>
    record(VkCommandBuffer cmd, std::span<const SkinDispatch> dispatches);

private:
    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ComputePipeline           pipeline_;

    std::vector<rhi::DescriptorBuffer> frame_descriptors_;
};

// Offscreen self-test: skin a small mesh (some vertices bound to a translated
// joint, others to an identity joint), read back the skinned vertex buffer, and
// verify positions/normals; then render the skinned buffer through the GBuffer
// pass to confirm it is usable as a vertex buffer.
[[nodiscard]] std::expected<void, core::Error>
run_skinning_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                            rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                            const std::string& cooked_shader_dir);

} // namespace engine::renderer
