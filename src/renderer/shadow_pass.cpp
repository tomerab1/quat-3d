#include "engine/renderer/shadow_pass.hpp"

#include <array>
#include <utility>

#include "engine/asset/mesh_asset.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

std::expected<ShadowPass, core::Error>
ShadowPass::create(const rhi::Device& device, rhi::PipelineCache& cache,
                   const std::string& cooked_shader_dir) {
    ShadowPass out;

    auto shader = cache.load(cooked_shader_dir + "/shadow.spv");
    if (!shader) return std::unexpected(shader.error());

    // Only the vertex position is needed; it lives at offset 0 of asset::Vertex.
    const VkVertexInputBindingDescription vbind{0, sizeof(asset::Vertex),
                                                VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 1> vattrs{
        {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, position)}}};
    const VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPushConstants)};

    rhi::GraphicsPipeline::CreateInfo info{};
    info.vertex = *shader;
    info.fragment = *shader;
    info.color_formats = {};                  // depth-only
    info.depth_format = shadow_map_format;
    info.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    info.cull_mode = VK_CULL_MODE_NONE;
    info.vertex_bindings = {&vbind, 1};
    info.vertex_attributes = vattrs;
    info.push_constants = {&pc, 1};

    auto pipeline = rhi::GraphicsPipeline::create(device, cache.handle(), info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);
    return out;
}

std::expected<rhi::ResourceHandle, core::Error>
ShadowPass::add_to_graph(rhi::RenderGraph& graph, const glm::mat4& light_view_proj,
                         std::span<const DrawItem> draws) {
    frame_draws_.clear();
    frame_draws_.reserve(draws.size());
    for (const DrawItem& d : draws) {
        if (d.mesh == nullptr || !d.mesh->valid()) continue;
        FrameDraw fd;
        fd.vertex_buffer = d.skinned_vertices != VK_NULL_HANDLE ? d.skinned_vertices
                                                                : d.mesh->vertex_buffer.handle();
        fd.index_buffer = d.mesh->index_buffer.handle();
        fd.submeshes = d.mesh->submeshes;
        fd.model = d.model;
        frame_draws_.push_back(fd);
    }

    const VkExtent2D extent{shadow_map_size, shadow_map_size};
    const rhi::ResourceHandle shadow = graph.create_transient_image(
        "shadow_map", shadow_map_format, extent,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    const glm::mat4 vp = light_view_proj;
    graph.add_pass("shadow", rhi::PassType::graphics)
        .writes(shadow, rhi::ResourceUsage::depth_attachment)
        .execute([this, shadow, extent, vp](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            const rhi::ResourceBinding depth = ctx.resolve(shadow);

            VkRenderingAttachmentInfo depth_attachment{};
            depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_attachment.imageView = depth.view;
            depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth_attachment.clearValue.depthStencil = {1.0F, 0};

            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 0;
            rendering.pDepthAttachment = &depth_attachment;

            vkCmdBeginRendering(cmd, &rendering);

            VkViewport viewport{};
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            viewport.minDepth = 0.0F;
            viewport.maxDepth = 1.0F;
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());

            for (const FrameDraw& fd : frame_draws_) {
                ShadowPushConstants push{};
                push.light_view_proj = vp;
                push.model = fd.model;
                vkCmdPushConstants(cmd, pipeline_.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(push), &push);
                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &fd.vertex_buffer, &offset);
                vkCmdBindIndexBuffer(cmd, fd.index_buffer, 0, VK_INDEX_TYPE_UINT32);
                for (const asset::SubMesh& sm : fd.submeshes) {
                    vkCmdDrawIndexed(cmd, sm.index_count, 1, sm.index_offset, 0, 0);
                }
            }

            vkCmdEndRendering(cmd);
        });

    return shadow;
}

} // namespace engine::renderer
