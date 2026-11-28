#include "engine/renderer/skinning_pass.hpp"

#include <array>
#include <string>
#include <utility>

#include <glm/mat4x4.hpp>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;

// Mirrors PushConstants in skinning.slang.
struct SkinningPushConstants {
    std::uint32_t vertex_count = 0;
    std::uint32_t joint_count = 0; // the shader clamps JOINTS_0 indices to this
    std::uint32_t write_previous = 0;
    std::uint32_t pad = 0;
};

constexpr VkDeviceSize kJointMatrixSize = sizeof(glm::mat4);

// skinning.slang's resources, by name; their binding slots come from the
// shader's reflection, in this order.
enum Resource : std::uint8_t {
    r_vertices_in,
    r_skin,
    r_joints,
    r_vertices_out,
    r_prev_joints,
    r_prev_positions,
    r_count
};
constexpr std::array<rhi::BindingRequest, r_count> kResources{{
    {"g_vertices_in", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_skin", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_joints", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_vertices_out", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_prev_joints", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_prev_positions", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
}};

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

    auto shader = cache.load(cooked_shader_dir + "/skinning.spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, kResources, kComputeStage);
    if (!bindings) return fail("skinning pass: " + bindings.error().message);
    for (const rhi::DescriptorBinding& b : *bindings) out.slots_.push_back(b.binding);
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, *bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    const VkPushConstantRange pc{kComputeStage, 0, sizeof(SkinningPushConstants)};
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    auto pipeline = cache.create_compute_pipeline(info);
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
        // No joint matrices: nothing valid to blend with (and the shader's
        // clamp needs at least one).
        const VkDeviceSize joint_count = d.joints_size / kJointMatrixSize;
        if (joint_count == 0 || d.vertex_count == 0) continue;
        const bool previous = d.prev_positions_out != 0;
        if (previous && d.prev_positions_out_size < VkDeviceSize{d.vertex_count} * 16) {
            return fail("skinning pass: previous-position buffer smaller than the vertex count");
        }
        auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
        if (!db) return std::unexpected(db.error());
        db->write_storage_buffer(slots_[r_vertices_in], d.vertices_in, d.vertices_in_size);
        db->write_storage_buffer(slots_[r_skin], d.skin, d.skin_size);
        db->write_storage_buffer(slots_[r_joints], d.joints, d.joints_size);
        db->write_storage_buffer(slots_[r_vertices_out], d.vertices_out, d.vertices_out_size);
        // Without previous data the shader never touches these slots; they
        // still need valid descriptors, so they alias the current buffers.
        db->write_storage_buffer(slots_[r_prev_joints],
                                 d.prev_joints != 0 ? d.prev_joints : d.joints, d.joints_size);
        db->write_storage_buffer(slots_[r_prev_positions],
                                 previous ? d.prev_positions_out : d.vertices_out,
                                 previous ? d.prev_positions_out_size : d.vertices_out_size);
        frame_descriptors_.push_back(std::move(*db));

        frame_descriptors_.back().bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.layout(), 0);
        const SkinningPushConstants push{d.vertex_count, static_cast<std::uint32_t>(joint_count),
                                         previous ? 1U : 0U, 0U};
        vkCmdPushConstants(cmd, pipeline_.layout(), kComputeStage, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (d.vertex_count + 63) / 64, 1, 1);
    }

    // Make the skinned vertices visible to the GBuffer pass's vertex fetch, and
    // the previous positions to its vertex shader (read by device address).
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask =
        VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
    return {};
}

} // namespace engine::renderer
