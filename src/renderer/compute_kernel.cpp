#include "compute_kernel.hpp"

#include <utility>

#include "engine/rhi/compute_pipeline.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer::detail {

void ComputeKernel::dispatch(VkCommandBuffer cmd, rhi::DescriptorBuffer& set, const void* push,
                             std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle());
    set.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout(), 0);
    if (push_bytes > 0 && push != nullptr) {
        vkCmdPushConstants(cmd, pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes,
                           push);
    }
    vkCmdDispatch(cmd, gx, gy, gz);
}

std::expected<ComputeKernel, core::Error>
build_compute_kernel(const rhi::Device& device, const rhi::DescriptorBufferFunctions& fns,
                     rhi::PipelineCache& cache, const std::string& cooked_shader_dir,
                     const char* shader, std::span<const rhi::BindingRequest> requests,
                     std::uint32_t push_bytes, const char* what, const char* entry) {
    auto module = cache.load(cooked_shader_dir + "/" + shader + ".spv");
    if (!module) return std::unexpected(module.error());
    auto bindings =
        rhi::reflected_bindings((*module)->reflection, requests, VK_SHADER_STAGE_COMPUTE_BIT);
    if (!bindings) {
        return std::unexpected(
            core::Error{std::string(what) + " (" + shader + "): " + bindings.error().message});
    }
    ComputeKernel out;
    out.push_bytes = push_bytes;
    out.slots.reserve(bindings->size());
    for (const rhi::DescriptorBinding& b : *bindings) out.slots.push_back(b.binding);
    auto layout = rhi::DescriptorSetLayout::create(device, fns, *bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout = std::move(*layout);
    const VkDescriptorSetLayout set_layout = out.layout.handle();
    const VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *module;
    info.entry = entry;
    info.set_layouts = {&set_layout, 1};
    if (push_bytes > 0) info.push_constants = {&pc, 1};
    auto pipeline = cache.create_compute_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline = std::move(*pipeline);
    return out;
}

std::expected<rhi::DescriptorBuffer, core::Error>
make_descriptor(const rhi::Device& device, rhi::GpuAllocator& allocator,
                const rhi::DescriptorBufferFunctions& fns, const ComputeKernel& kernel) {
    return rhi::DescriptorBuffer::create(device, allocator, fns, kernel.layout);
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                   VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                   VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                   VkAccessFlags2 dst_access, std::uint32_t mips, std::uint32_t layers,
                   std::uint32_t base_mip) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base_mip, mips, 0, layers};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage,
                    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                    VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace engine::renderer::detail
