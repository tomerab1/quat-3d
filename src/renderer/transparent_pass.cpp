#include "engine/renderer/transparent_pass.hpp"

#include <array>
#include <cstddef>
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

constexpr VkShaderStageFlags kMaterialStages = VK_SHADER_STAGE_FRAGMENT_BIT;

// Material (uniform params at 0, five textures at 1..5, sampler at 6) plus the
// copied scene-colour texture at 7 + its sampler at 8 (for glass transmission).
[[nodiscard]] std::array<rhi::DescriptorBinding, 9> material_bindings() {
    using rhi::DescriptorBinding;
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kMaterialStages},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kMaterialStages},
        {0, 7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 8, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kMaterialStages},
    }};
}

} // namespace

std::expected<TransparentPass, core::Error>
TransparentPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                        rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                        const std::string& cooked_shader_dir) {
    TransparentPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("transparent pass: descriptor buffer functions unavailable");
    }

    const auto bindings = material_bindings();
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, bindings);
    if (!layout) return std::unexpected(layout.error());
    out.material_layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/transparent.spv");
    if (!shader) return std::unexpected(shader.error());

    const VkVertexInputBindingDescription vbind{0, sizeof(asset::Vertex),
                                                VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 4> vattrs{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(asset::Vertex, uv)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(asset::Vertex, tangent)},
    }};
    const std::array<VkFormat, 1> color_formats{hdr_color_format};
    const VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(TransparentPushConstants)};
    const VkDescriptorSetLayout set_layout = out.material_layout_.handle();

    rhi::GraphicsPipeline::CreateInfo info{};
    info.vertex = *shader;
    info.fragment = *shader;
    info.color_formats = color_formats;
    info.depth_format = gbuffer_depth_format;
    info.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    info.depth_write = false;  // read-only depth test against the opaque depth
    info.alpha_blend = true;
    info.cull_mode = VK_CULL_MODE_NONE;
    info.vertex_bindings = {&vbind, 1};
    info.vertex_attributes = vattrs;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};

    auto pipeline = rhi::GraphicsPipeline::create(device, cache.handle(), info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    auto sampler = rhi::create_sampler(device.handle());
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    auto scene_sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!scene_sampler) return std::unexpected(scene_sampler.error());
    out.scene_sampler_ = std::move(*scene_sampler);

    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    auto img = rhi::upload_device_image(allocator, transfer, white.data(), white.size(),
                                        VkExtent2D{1, 1}, VK_FORMAT_R8G8B8A8_UNORM);
    if (!img) return std::unexpected(img.error());
    auto view = rhi::create_image_view(device.handle(), img->handle(), VK_FORMAT_R8G8B8A8_UNORM);
    if (!view) return std::unexpected(view.error());
    out.fallback_texture_.image = std::move(*img);
    out.fallback_texture_.view = std::move(*view);
    out.fallback_texture_.format = VK_FORMAT_R8G8B8A8_UNORM;

    auto mat = asset::upload_material(asset::PbrMaterialParams{}, asset::MaterialTextures{},
                                      allocator, transfer);
    if (!mat) return std::unexpected(mat.error());
    out.default_material_ = std::move(*mat);

    return out;
}

VkImageView TransparentPass::texture_view_or_fallback(
    const asset::AssetHandle<asset::TextureAsset>& handle) const {
    if (handle.valid() && handle.is_loaded() && handle->valid()) {
        return handle->view.handle();
    }
    return fallback_texture_.view.handle();
}

std::expected<void, core::Error>
TransparentPass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr,
                              rhi::ResourceHandle depth, VkExtent2D extent,
                              const glm::mat4& view_proj, const DirectionalLightParams& light,
                              const glm::vec3& camera_pos, std::span<const DrawItem> draws) {
    frame_descriptors_.clear();
    frame_draws_.clear();

    for (const DrawItem& d : draws) {
        if (d.mesh == nullptr || !d.mesh->valid()) continue;
        const asset::MaterialAsset& mat =
            (d.material != nullptr && d.material->valid()) ? *d.material : default_material_;

        auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, material_layout_);
        if (!db) return std::unexpected(db.error());
        db->write_uniform_buffer(0, mat.param_address, sizeof(asset::PbrMaterialParams));
        constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        db->write_sampled_image(1, texture_view_or_fallback(mat.textures.base_color), ro);
        db->write_sampled_image(2, texture_view_or_fallback(mat.textures.normal), ro);
        db->write_sampled_image(3, texture_view_or_fallback(mat.textures.metallic_roughness), ro);
        db->write_sampled_image(4, texture_view_or_fallback(mat.textures.emissive), ro);
        db->write_sampled_image(5, texture_view_or_fallback(mat.textures.occlusion), ro);
        db->write_sampler(6, sampler_.handle());

        FrameDraw fd;
        fd.vertex_buffer = d.skinned_vertices != VK_NULL_HANDLE ? d.skinned_vertices
                                                                : d.mesh->vertex_buffer.handle();
        fd.index_buffer = d.mesh->index_buffer.handle();
        fd.submeshes = d.mesh->submeshes;
        fd.model = d.model;
        fd.descriptor_index = static_cast<std::uint32_t>(frame_descriptors_.size());
        frame_descriptors_.push_back(std::move(*db));
        frame_draws_.push_back(fd);
    }

    TransparentPushConstants push{};
    push.view_proj = view_proj;
    push.camera_pos = glm::vec4(camera_pos, 1.0F);
    // light_direction.w / ambient.w carry the render extent (for screen UVs).
    push.light_direction = glm::vec4(glm::vec3(light.direction), static_cast<float>(extent.width));
    push.light_color = light.color;
    push.ambient = glm::vec4(glm::vec3(light.ambient), static_cast<float>(extent.height));

    // Snapshot the opaque HDR so transmissive (glass) surfaces can sample what is
    // behind them.
    const rhi::ResourceHandle scene_color = graph.create_transient_image(
        "scene_color", hdr_color_format, extent,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    graph.add_pass("scene_copy", rhi::PassType::transfer)
        .reads(hdr, rhi::ResourceUsage::transfer_src)
        .writes(scene_color, rhi::ResourceUsage::transfer_dst)
        .execute([hdr, scene_color, extent](rhi::PassContext& ctx) {
            const rhi::ResourceBinding src = ctx.resolve(hdr);
            const rhi::ResourceBinding dst = ctx.resolve(scene_color);
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {extent.width, extent.height, 1};
            vkCmdCopyImage(ctx.cmd(), src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        });

    graph.add_pass("transparent", rhi::PassType::graphics)
        .reads(depth, rhi::ResourceUsage::depth_attachment)
        .reads(scene_color, rhi::ResourceUsage::sampled)
        .writes(hdr, rhi::ResourceUsage::color_attachment)
        .execute([this, hdr, depth, scene_color, extent, push](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            const rhi::ResourceBinding color = ctx.resolve(hdr);
            const rhi::ResourceBinding depth_b = ctx.resolve(depth);

            // Finish each draw's descriptor set with the resolved scene-colour view.
            constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            const VkImageView scene_view = ctx.resolve(scene_color).view;
            for (rhi::DescriptorBuffer& db : frame_descriptors_) {
                db.write_sampled_image(7, scene_view, ro);
                db.write_sampler(8, scene_sampler_.handle());
            }

            VkRenderingAttachmentInfo color_attachment{};
            color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color_attachment.imageView = color.view;
            color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // keep the opaque result
            color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingAttachmentInfo depth_attachment{};
            depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth_attachment.imageView = depth_b.view;
            depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_NONE;

            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color_attachment;
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
                frame_descriptors_[fd.descriptor_index].bind(
                    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0);
                TransparentPushConstants p = push;
                p.model = fd.model;
                vkCmdPushConstants(cmd, pipeline_.layout(),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(p), &p);
                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &fd.vertex_buffer, &offset);
                vkCmdBindIndexBuffer(cmd, fd.index_buffer, 0, VK_INDEX_TYPE_UINT32);
                for (const asset::SubMesh& sm : fd.submeshes) {
                    vkCmdDrawIndexed(cmd, sm.index_count, 1, sm.index_offset, 0, 0);
                }
            }

            vkCmdEndRendering(cmd);
        });

    return {};
}

} // namespace engine::renderer
