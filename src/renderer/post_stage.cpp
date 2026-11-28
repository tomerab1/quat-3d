#include "post_stage.hpp"

#include <utility>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer::post {

namespace {
constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;
} // namespace

std::expected<ComputeStage, core::Error>
make_compute_stage(const rhi::Device& device, const rhi::DescriptorBufferFunctions& fns,
                   rhi::PipelineCache& cache, const std::string& dir, const std::string& name,
                   std::span<const rhi::BindingRequest> resources, std::uint32_t push_size) {
    auto shader = cache.load(dir + "/" + name + ".spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, resources, kComputeStage);
    if (!bindings) return std::unexpected(core::Error{name + ": " + bindings.error().message});
    ComputeStage out;
    for (const rhi::DescriptorBinding& b : *bindings) out.slots.push_back(b.binding);
    auto layout = rhi::DescriptorSetLayout::create(device, fns, *bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout = std::move(*layout);

    const VkDescriptorSetLayout set_layout = out.layout.handle();
    const VkPushConstantRange pc{kComputeStage, 0, push_size};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    auto pipeline = cache.create_compute_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline = std::move(*pipeline);
    return out;
}

std::expected<std::size_t, core::Error>
DescriptorPool::add(const rhi::Device& device, rhi::GpuAllocator& allocator,
                    const rhi::DescriptorBufferFunctions& fns, const ComputeStage& stage) {
    auto db = rhi::DescriptorBuffer::create(device, allocator, fns, stage.layout);
    if (!db) return std::unexpected(db.error());
    buffers_.push_back(std::move(*db));
    return buffers_.size() - 1;
}

void dispatch(VkCommandBuffer cmd, const ComputeStage& stage, rhi::DescriptorBuffer& descriptors,
              const void* push, std::uint32_t push_size, std::uint32_t width,
              std::uint32_t height) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, stage.pipeline.handle());
    descriptors.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, stage.pipeline.layout(), 0);
    vkCmdPushConstants(cmd, stage.pipeline.layout(), kComputeStage, 0, push_size, push);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
}

} // namespace engine::renderer::post
