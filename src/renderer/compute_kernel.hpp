#pragma once

// A compute pipeline whose single descriptor-set layout comes from shader
// reflection (bindings by resource name, never hard-coded indices), plus the
// small helpers the R21 passes (HdriEnvironment, SsrPass) record with.
// Renderer-internal.

#include <array>
#include <cstddef>
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
} // namespace engine::rhi

namespace engine::renderer::detail {

struct ComputeKernel {
    rhi::DescriptorSetLayout   layout;
    rhi::ManagedPipeline       pipeline;
    std::vector<std::uint32_t> slots; // reflected binding of each request, in order
    std::uint32_t              push_bytes = 0;

    // Binds the pipeline and `set`, pushes `push` (push_bytes) and dispatches.
    void dispatch(VkCommandBuffer cmd, rhi::DescriptorBuffer& set, const void* push,
                  std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) const;
};

// Loads `spv` (cooked_shader_dir-relative name without extension) and builds
// the kernel for `requests` from its entry point `entry`. `what` prefixes
// errors.
[[nodiscard]] std::expected<ComputeKernel, core::Error>
build_compute_kernel(const rhi::Device& device, const rhi::DescriptorBufferFunctions& fns,
                     rhi::PipelineCache& cache, const std::string& cooked_shader_dir,
                     const char* shader, std::span<const rhi::BindingRequest> requests,
                     std::uint32_t push_bytes, const char* what,
                     const char* entry = "compute_main");

// One descriptor set (buffer) for `kernel`.
[[nodiscard]] std::expected<rhi::DescriptorBuffer, core::Error>
make_descriptor(const rhi::Device& device, rhi::GpuAllocator& allocator,
                const rhi::DescriptorBufferFunctions& fns, const ComputeKernel& kernel);

[[nodiscard]] constexpr std::uint32_t groups_of(std::uint32_t n, std::uint32_t size = 8) {
    return (n + size - 1) / size;
}

// Image barrier over `mips` x `layers` from mip `base_mip` (colour aspect).
void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access, std::uint32_t mips = 1, std::uint32_t layers = 1,
                   std::uint32_t base_mip = 0);

// Global memory barrier (compute writes -> later reads, buffers included).
void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage,
                    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                    VkAccessFlags2 dst_access);

} // namespace engine::renderer::detail
