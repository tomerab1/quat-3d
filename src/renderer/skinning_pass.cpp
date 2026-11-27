#include "engine/renderer/skinning_pass.hpp"

#include <array>
#include <string>
#include <utility>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;

struct SkinningPushConstants {
    std::uint32_t vertex_count = 0;
};

// Set 0: vertices-in, skin, joints (read) and vertices-out (write), all storage
// buffers. Matches skinning.slang.
[[nodiscard]] std::array<rhi::DescriptorBinding, 4> skinning_bindings() {
    using rhi::DescriptorBinding;
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
    }};
}

} // namespace

std::expected<SkinningPass, core::Error>
SkinningPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                     rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    SkinningPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("skinning pass: descriptor buffer functions unavailable");
    }

    const auto bindings = skinning_bindings();
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/skinning.spv");
    if (!shader) return std::unexpected(shader.error());

    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    const VkPushConstantRange pc{kComputeStage, 0, sizeof(SkinningPushConstants)};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    auto pipeline = rhi::ComputePipeline::create(device, cache.handle(), info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    return out;
}

std::expected<void, core::Error>
SkinningPass::record(VkCommandBuffer cmd, std::span<const SkinDispatch> dispatches) {
    frame_descriptors_.clear();
    frame_descriptors_.reserve(dispatches.size());

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());

    for (const SkinDispatch& d : dispatches) {
        auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
        if (!db) return std::unexpected(db.error());
        db->write_storage_buffer(0, d.vertices_in, d.vertices_in_size);
        db->write_storage_buffer(1, d.skin, d.skin_size);
        db->write_storage_buffer(2, d.joints, d.joints_size);
        db->write_storage_buffer(3, d.vertices_out, d.vertices_out_size);
        frame_descriptors_.push_back(std::move(*db));

        frame_descriptors_.back().bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
        SkinningPushConstants push{d.vertex_count};
        vkCmdPushConstants(cmd, pipeline_.layout(), kComputeStage, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (d.vertex_count + 63) / 64, 1, 1);
    }

    // Make the skinned vertices visible to the GBuffer pass's vertex fetch.
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
    return {};
}

} // namespace engine::renderer
