#include "engine/rhi/graphics_pipeline.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <utility>
#include <vector>

#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::rhi {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// Union of the reflected push-constant ranges across the supplied shaders. Slang
// reports a single global range spanning the constant buffer; we collapse it to
// one VERTEX|FRAGMENT range covering [0, max_end) so it is visible to both
// stages. Empty when no shader declares push constants (the triangle case).
[[nodiscard]] std::vector<VkPushConstantRange>
push_constant_ranges(const LoadedShader& vertex, const LoadedShader& fragment) {
    std::uint32_t max_end = 0;
    for (const LoadedShader* s : {&vertex, &fragment}) {
        for (const auto& pc : s->reflection.push_constant_ranges) {
            max_end = std::max(max_end, pc.offset + pc.size);
        }
    }
    if (max_end == 0) return {};
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = max_end;
    return {range};
}

} // namespace

std::expected<GraphicsPipeline, core::Error>
GraphicsPipeline::create(const Device& device, VkPipelineCache cache, const CreateInfo& info) {
    if (info.vertex == nullptr || info.fragment == nullptr) {
        return fail("GraphicsPipeline::create: vertex/fragment shader missing");
    }
    // A pipeline must write somewhere: at least one colour target, or depth (a
    // depth-only pass such as the shadow map).
    if (info.color_formats.empty() && info.depth_format == VK_FORMAT_UNDEFINED) {
        return fail("GraphicsPipeline::create: no colour or depth attachment formats");
    }

    const VkDevice vk_device = device.handle();

    // ---- Pipeline layout (descriptor set layouts + push constants) ----------
    // Explicit push-constant ranges (info.push_constants) win; otherwise derive
    // a single VERTEX|FRAGMENT range from shader reflection.
    std::vector<VkPushConstantRange> reflected;
    std::span<const VkPushConstantRange> pc = info.push_constants;
    if (pc.empty()) {
        reflected = push_constant_ranges(*info.vertex, *info.fragment);
        pc = reflected;
    }
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<std::uint32_t>(info.set_layouts.size());
    layout_info.pSetLayouts = info.set_layouts.empty() ? nullptr : info.set_layouts.data();
    layout_info.pushConstantRangeCount = static_cast<std::uint32_t>(pc.size());
    layout_info.pPushConstantRanges = pc.empty() ? nullptr : pc.data();

    GraphicsPipeline out;
    out.device_ = vk_device;
    if (vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &out.layout_) != VK_SUCCESS) {
        return fail("GraphicsPipeline::create: vkCreatePipelineLayout failed");
    }

    // ---- Shader stages ------------------------------------------------------
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = info.vertex->module;
    stages[0].pName = info.vertex_entry;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = info.fragment->module;
    stages[1].pName = info.fragment_entry;

    // ---- Fixed-function state ----------------------------------------------
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount =
        static_cast<std::uint32_t>(info.vertex_bindings.size());
    vertex_input.pVertexBindingDescriptions =
        info.vertex_bindings.empty() ? nullptr : info.vertex_bindings.data();
    vertex_input.vertexAttributeDescriptionCount =
        static_cast<std::uint32_t>(info.vertex_attributes.size());
    vertex_input.pVertexAttributeDescriptions =
        info.vertex_attributes.empty() ? nullptr : info.vertex_attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = info.cull_mode;
    raster.frontFace = info.front_face;
    raster.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    const bool has_depth = info.depth_format != VK_FORMAT_UNDEFINED;
    depth_stencil.depthTestEnable = has_depth ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = (has_depth && info.depth_write) ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = info.depth_compare_op;

    // One blend attachment per colour target. Opaque writes are unblended; the
    // transparent pass uses standard SRC_ALPHA / ONE_MINUS_SRC_ALPHA blending.
    VkPipelineColorBlendAttachmentState blend_template{};
    blend_template.blendEnable = info.alpha_blend ? VK_TRUE : VK_FALSE;
    blend_template.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_template.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_template.colorBlendOp = VK_BLEND_OP_ADD;
    blend_template.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_template.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_template.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_template.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                    | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(
        info.color_formats.size(), blend_template);

    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = static_cast<std::uint32_t>(blend_attachments.size());
    color_blend.pAttachments = blend_attachments.data();

    std::array<VkDynamicState, 3> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = info.dynamic_cull_mode ? 3u : 2u;
    dynamic_state.pDynamicStates = dynamic_states.data();

    // ---- Dynamic rendering: declare attachment formats (no VkRenderPass) ----
    VkPipelineRenderingCreateInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_info.colorAttachmentCount = static_cast<std::uint32_t>(info.color_formats.size());
    rendering_info.pColorAttachmentFormats = info.color_formats.data();
    rendering_info.depthAttachmentFormat = info.depth_format;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering_info;
    // Pipelines that bind descriptor-buffer set layouts must declare it. Not set
    // on the classic-sets fallback backend, which binds plain descriptor sets.
    if (!info.set_layouts.empty() && device.uses_descriptor_buffer()) {
        pipeline_info.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    }
    pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = out.layout_;

    if (vkCreateGraphicsPipelines(vk_device, cache, 1, &pipeline_info, nullptr, &out.pipeline_)
        != VK_SUCCESS) {
        return fail("GraphicsPipeline::create: vkCreateGraphicsPipelines failed");
    }
    return out;
}

void GraphicsPipeline::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, layout_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
}

GraphicsPipeline::~GraphicsPipeline() { destroy(); }

GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept { *this = std::move(other); }

GraphicsPipeline& GraphicsPipeline::operator=(GraphicsPipeline&& other) noexcept {
    if (this != &other) {
        destroy();
        device_   = std::exchange(other.device_, VK_NULL_HANDLE);
        layout_   = std::exchange(other.layout_, VK_NULL_HANDLE);
        pipeline_ = std::exchange(other.pipeline_, VK_NULL_HANDLE);
    }
    return *this;
}

} // namespace engine::rhi
