#pragma once

// Private to the renderer: one compute stage of a multi-dispatch post pass
// (R20 depth of field and motion blur) — a shader, its set layout and the
// binding slot of each resource, all from the shader's reflection (bindings by
// name, CLAUDE.md), plus a per-frame descriptor buffer pool.

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/shader_reflection.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
}

namespace engine::renderer::post {

struct ComputeStage {
    rhi::DescriptorSetLayout   layout;
    rhi::ManagedPipeline       pipeline;
    std::vector<std::uint32_t> slots; // binding of each requested resource, in order
};

// Loads `<dir>/<name>.spv` and builds the stage from its reflection.
[[nodiscard]] std::expected<ComputeStage, core::Error>
make_compute_stage(const rhi::Device& device, const rhi::DescriptorBufferFunctions& fns,
                   rhi::PipelineCache& cache, const std::string& dir, const std::string& name,
                   std::span<const rhi::BindingRequest> resources, std::uint32_t push_size);

// Descriptor buffers created while declaring a frame's passes; they must live
// until the graph has executed (cleared at the next declaration).
class DescriptorPool {
public:
    void reset() { buffers_.clear(); }
    // A new descriptor buffer for `stage`, or the creation error.
    [[nodiscard]] std::expected<std::size_t, core::Error>
    add(const rhi::Device& device, rhi::GpuAllocator& allocator,
        const rhi::DescriptorBufferFunctions& fns, const ComputeStage& stage);
    [[nodiscard]] rhi::DescriptorBuffer& operator[](std::size_t i) { return buffers_[i]; }

private:
    std::vector<rhi::DescriptorBuffer> buffers_;
};

// Binds the stage, its descriptor buffer and push constants, and dispatches
// 8 x 8 groups over `width` x `height` threads.
void dispatch(VkCommandBuffer cmd, const ComputeStage& stage, rhi::DescriptorBuffer& descriptors,
              const void* push, std::uint32_t push_size, std::uint32_t width,
              std::uint32_t height);

} // namespace engine::renderer::post
