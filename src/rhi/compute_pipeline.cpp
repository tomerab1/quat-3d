#include "engine/rhi/compute_pipeline.hpp"

#include <string>
#include <utility>

#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::rhi {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

} // namespace

std::expected<ComputePipeline, core::Error>
ComputePipeline::create(const Device& device, VkPipelineCache cache, const CreateInfo& info) {
    if (info.shader == nullptr) {
        return fail("ComputePipeline::create: shader missing");
    }
    const VkDevice vk_device = device.handle();

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<std::uint32_t>(info.set_layouts.size());
    layout_info.pSetLayouts = info.set_layouts.empty() ? nullptr : info.set_layouts.data();
    layout_info.pushConstantRangeCount = static_cast<std::uint32_t>(info.push_constants.size());
    layout_info.pPushConstantRanges =
        info.push_constants.empty() ? nullptr : info.push_constants.data();

    ComputePipeline out;
    out.device_ = vk_device;
    if (vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &out.layout_) != VK_SUCCESS) {
        return fail("ComputePipeline::create: vkCreatePipelineLayout failed");
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = info.shader->module;
    stage.pName = info.entry;

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = out.layout_;
    // Not set on the classic-sets fallback backend (plain descriptor sets).
    if (!info.set_layouts.empty() && device.uses_descriptor_buffer()) {
        pipeline_info.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    }

    if (vkCreateComputePipelines(vk_device, cache, 1, &pipeline_info, nullptr, &out.pipeline_)
        != VK_SUCCESS) {
        return fail("ComputePipeline::create: vkCreateComputePipelines failed");
    }
    return out;
}

void ComputePipeline::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, layout_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
}

ComputePipeline::~ComputePipeline() { destroy(); }

ComputePipeline::ComputePipeline(ComputePipeline&& other) noexcept { *this = std::move(other); }

ComputePipeline& ComputePipeline::operator=(ComputePipeline&& other) noexcept {
    if (this != &other) {
        destroy();
        device_   = std::exchange(other.device_, VK_NULL_HANDLE);
        layout_   = std::exchange(other.layout_, VK_NULL_HANDLE);
        pipeline_ = std::exchange(other.pipeline_, VK_NULL_HANDLE);
    }
    return *this;
}

} // namespace engine::rhi
