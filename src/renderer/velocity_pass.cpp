#include "engine/renderer/velocity_pass.hpp"

#include <utility>

#include "engine/renderer/velocity.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

// Mirrors PushConstants in velocity_background.slang (144 bytes).
struct BackgroundPush {
    glm::mat4 inv_view_proj{1.0F};
    glm::mat4 prev_view_proj{1.0F};
    glm::vec4 params{0.0F}; // xy jitter (NDC), zw 1 / extent
};
static_assert(sizeof(BackgroundPush) == 144, "must match velocity_background.slang");

constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

} // namespace

std::expected<VelocityPass, core::Error>
VelocityPass::create(const rhi::Device& device, rhi::PipelineCache& cache,
                     const std::string& cooked_shader_dir) {
    VelocityPass out;
    // The shader binds nothing, but it draws while other passes' descriptor
    // buffers are bound: an (empty) descriptor-buffer set layout gives the
    // pipeline the descriptor-buffer flag that requires.
    const rhi::DescriptorBufferFunctions fns = rhi::DescriptorBufferFunctions::load(device.handle());
    auto layout = rhi::DescriptorSetLayout::create(device, fns, {});
    if (!layout) return std::unexpected(layout.error());
    out.empty_layout_ = std::move(*layout);
    const VkDescriptorSetLayout set_layout = out.empty_layout_.handle();

    auto shader = cache.load(cooked_shader_dir + "/velocity_background.spv");
    if (!shader) return std::unexpected(shader.error());
    const VkFormat color = velocity_format;
    const VkPushConstantRange pc{kStages, 0, sizeof(BackgroundPush)};
    rhi::GraphicsPipeline::CreateInfo info{};
    info.set_layouts = {&set_layout, 1};
    info.vertex = *shader;
    info.fragment = *shader;
    info.color_formats = {&color, 1};
    info.depth_format = gbuffer_depth_format;
    info.depth_compare_op = VK_COMPARE_OP_EQUAL; // only where depth is still the clear (1)
    info.depth_write = false;
    info.push_constants = {&pc, 1};
    auto pipeline = cache.create_graphics_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);
    return out;
}

void VelocityPass::add_background_to_graph(rhi::RenderGraph& graph, const GBufferTargets& gbuffer,
                                           VkExtent2D extent, const glm::mat4& view_proj) const {
    BackgroundPush push{};
    push.inv_view_proj = glm::inverse(view_proj);
    push.prev_view_proj = gbuffer.motion.prev_view_proj;
    push.params = glm::vec4(gbuffer.motion.jitter_ndc, 1.0F / static_cast<float>(extent.width),
                            1.0F / static_cast<float>(extent.height));
    const rhi::ResourceHandle velocity = gbuffer.velocity;
    const rhi::ResourceHandle depth = gbuffer.depth;
    graph.add_pass("velocity_background", rhi::PassType::graphics)
        .writes(velocity, rhi::ResourceUsage::color_attachment)
        .reads(depth, rhi::ResourceUsage::depth_attachment)
        .execute([this, velocity, depth, extent, push](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = ctx.resolve(velocity).view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingAttachmentInfo depth_info{};
            depth_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_info.imageView = ctx.resolve(depth).view;
            depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth_info.storeOp = VK_ATTACHMENT_STORE_OP_NONE; // read-only: test, no write

            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            rendering.pDepthAttachment = &depth_info;
            vkCmdBeginRendering(cmd, &rendering);

            VkViewport viewport{};
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            viewport.maxDepth = 1.0F;
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
            vkCmdPushConstants(cmd, pipeline_.layout(), kStages, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
        });
}

} // namespace engine::renderer
