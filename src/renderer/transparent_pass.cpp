#include "engine/renderer/transparent_pass.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kMaterialStages = VK_SHADER_STAGE_FRAGMENT_BIT;

// Material (uniform params at 0, five textures at 1..5, sampler at 6), the
// scene-colour mip chain at 7 + its sampler at 8 (glass transmission), the
// IBL maps at 9..12 (environment reflection on glass), and the volume
// thickness texture at 13.
[[nodiscard]] std::array<rhi::DescriptorBinding, 14> material_bindings() {
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
        {0, 9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 12, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kMaterialStages},
        {0, 13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
    }};
}

// A single-mip, single-layer sync2 image barrier (colour aspect).
[[nodiscard]] VkImageMemoryBarrier2
mip_barrier(VkImage image, std::uint32_t mip, VkImageLayout old_layout, VkImageLayout new_layout,
            VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
            VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
    return b;
}

void submit_barriers(VkCommandBuffer cmd, std::span<const VkImageMemoryBarrier2> barriers) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
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
    info.vertex_bindings = {&vbind, 1};
    info.vertex_attributes = vattrs;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};

    // Blend pipeline: glTF BLEND. Read-only depth, two-sided, SRC_ALPHA blend.
    info.depth_write = false;
    info.alpha_blend = true;
    info.cull_mode = VK_CULL_MODE_NONE;
    auto blend = rhi::GraphicsPipeline::create(device, cache.handle(), info);
    if (!blend) return std::unexpected(blend.error());
    out.blend_pipeline_ = std::move(*blend);

    // Transmission pipeline: glTF KHR_materials_transmission is NOT alpha
    // blending — glass writes depth, replaces the pixel (the shader composites
    // the refracted background itself) and is back-face culled unless the
    // material is doubleSided (cull mode is dynamic, set per draw).
    info.depth_write = true;
    info.alpha_blend = false;
    info.dynamic_cull_mode = true;
    auto transmission = rhi::GraphicsPipeline::create(device, cache.handle(), info);
    if (!transmission) return std::unexpected(transmission.error());
    out.transmission_pipeline_ = std::move(*transmission);

    auto sampler = rhi::create_sampler(device.handle());
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    auto scene_sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge,
                                             kSceneColorMips);
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

    auto fallback_ibl = IblMaps::create_fallback(device, allocator);
    if (!fallback_ibl) return std::unexpected(fallback_ibl.error());
    out.fallback_ibl_ = std::move(*fallback_ibl);

    return out;
}

VkImageView TransparentPass::texture_view_or_fallback(
    const asset::AssetHandle<asset::TextureAsset>& handle) const {
    if (handle.valid() && handle.is_loaded() && handle->valid()) {
        return handle->view.handle();
    }
    return fallback_texture_.view.handle();
}

std::expected<void, core::Error> TransparentPass::ensure_scene_color(VkExtent2D extent) {
    if (scene_color_.handle() != VK_NULL_HANDLE && scene_color_extent_.width == extent.width &&
        scene_color_extent_.height == extent.height) {
        return {};
    }
    const std::uint32_t full_chain =
        static_cast<std::uint32_t>(std::bit_width(std::max(extent.width, extent.height)));
    const std::uint32_t mips = std::min(kSceneColorMips, full_chain);

    auto image = allocator_->create_image(hdr_color_format, extent,
                                          VK_IMAGE_USAGE_SAMPLED_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                          mips);
    if (!image) return std::unexpected(image.error());
    auto view = rhi::create_image_view(device_->handle(), image->handle(), hdr_color_format,
                                       VK_IMAGE_ASPECT_COLOR_BIT, mips);
    if (!view) return std::unexpected(view.error());

    scene_color_ = std::move(*image);
    scene_color_view_ = std::move(*view);
    scene_color_extent_ = extent;
    scene_color_mips_ = mips;
    return {};
}

std::expected<void, core::Error>
TransparentPass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr,
                              rhi::ResourceHandle depth, VkExtent2D extent,
                              const glm::mat4& view_proj, const DirectionalLightParams& light,
                              const glm::vec3& camera_pos, std::span<const DrawItem> draws,
                              IblViewSet ibl) {
    frame_descriptors_.clear();
    frame_draws_.clear();

    if (auto r = ensure_scene_color(extent); !r) return r;

    // Non-owning view set; falls back to the member black maps when empty.
    const IblViewSet maps = ibl.valid() ? ibl : fallback_ibl_.views();

    bool any_transmissive = false;
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
        db->write_sampled_image(7, scene_color_view_.handle(), ro);
        db->write_sampler(8, scene_sampler_.handle());
        db->write_sampled_image(9, maps.irradiance, ro);
        db->write_sampled_image(10, maps.prefiltered, ro);
        db->write_sampled_image(11, maps.brdf_lut, ro);
        db->write_sampler(12, maps.sampler);
        db->write_sampled_image(13, texture_view_or_fallback(mat.textures.thickness), ro);

        FrameDraw fd;
        fd.vertex_buffer = d.skinned_vertices != VK_NULL_HANDLE ? d.skinned_vertices
                                                                : d.mesh->vertex_buffer.handle();
        fd.index_buffer = d.mesh->index_buffer.handle();
        fd.submeshes = d.mesh->submeshes;
        fd.model = d.model;
        fd.descriptor_index = static_cast<std::uint32_t>(frame_descriptors_.size());
        fd.transmissive = (mat.params.flags & asset::material_transmission) != 0;
        fd.cull_mode = (mat.params.flags & asset::material_double_sided) != 0
                           ? VK_CULL_MODE_NONE
                           : VK_CULL_MODE_BACK_BIT;
        any_transmissive = any_transmissive || fd.transmissive;
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

    // Snapshot the opaque HDR into the persistent scene-colour image and build
    // its blurred mip chain, so transmissive (glass) surfaces can sample what is
    // behind them at a roughness-dependent blur. Skipped when nothing refracts.
    // The graph's barriers cover mip 0 only — mips 1.. are managed manually here
    // and handed to the fragment stage in SHADER_READ_ONLY.
    rhi::ResourceHandle scene_handle{};
    if (any_transmissive) {
        scene_handle = graph.import_image("scene_color", scene_color_.handle(),
                                          scene_color_view_.handle(), hdr_color_format, extent,
                                          VK_IMAGE_LAYOUT_UNDEFINED, // fully overwritten below
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        graph.add_pass("scene_copy", rhi::PassType::transfer)
            .reads(hdr, rhi::ResourceUsage::transfer_src)
            .writes(scene_handle, rhi::ResourceUsage::transfer_dst)
            .execute([this, hdr, extent](rhi::PassContext& ctx) {
                const VkCommandBuffer cmd = ctx.cmd();
                const VkImage src = ctx.resolve(hdr).image;
                const VkImage dst = scene_color_.handle();
                const std::uint32_t mips = scene_color_mips_;

                // Mips 1.. -> TRANSFER_DST (contents discarded; rewritten below).
                std::vector<VkImageMemoryBarrier2> barriers;
                for (std::uint32_t m = 1; m < mips; ++m) {
                    barriers.push_back(mip_barrier(
                        dst, m, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT));
                }
                if (!barriers.empty()) submit_barriers(cmd, barriers);

                // HDR -> mip 0 (already TRANSFER_DST via the graph).
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.extent = {extent.width, extent.height, 1};
                vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                // Downsample chain: mip m-1 -> mip m with linear blits.
                std::int32_t pw = static_cast<std::int32_t>(extent.width);
                std::int32_t ph = static_cast<std::int32_t>(extent.height);
                for (std::uint32_t m = 1; m < mips; ++m) {
                    const VkImageMemoryBarrier2 to_src = mip_barrier(
                        dst, m - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
                    submit_barriers(cmd, {&to_src, 1});

                    const std::int32_t cw = std::max(pw / 2, 1);
                    const std::int32_t ch = std::max(ph / 2, 1);
                    VkImageBlit blit{};
                    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
                    blit.srcOffsets[1] = {pw, ph, 1};
                    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
                    blit.dstOffsets[1] = {cw, ch, 1};
                    vkCmdBlitImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                   VK_FILTER_LINEAR);
                    pw = cw;
                    ph = ch;
                }

                // Hand back: mip 0 to TRANSFER_DST (the layout the graph believes
                // and transitions to SHADER_READ_ONLY itself); mips 1.. straight
                // to SHADER_READ_ONLY for the fragment stage.
                barriers.clear();
                barriers.push_back(mip_barrier(
                    dst, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT));
                for (std::uint32_t m = 1; m < mips; ++m) {
                    const bool last = m == mips - 1;
                    barriers.push_back(mip_barrier(
                        dst, m,
                        last ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                             : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                        last ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
                }
                submit_barriers(cmd, barriers);
            });
    }

    auto pass = graph.add_pass("transparent", rhi::PassType::graphics);
    pass.writes(depth, rhi::ResourceUsage::depth_attachment) // glass writes depth
        .writes(hdr, rhi::ResourceUsage::color_attachment);
    if (any_transmissive) {
        pass.reads(scene_handle, rhi::ResourceUsage::sampled);
    }
    pass.execute([this, hdr, depth, extent, push](rhi::PassContext& ctx) {
        const VkCommandBuffer cmd = ctx.cmd();
        const rhi::ResourceBinding color = ctx.resolve(hdr);
        const rhi::ResourceBinding depth_b = ctx.resolve(depth);

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
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // transmission writes depth

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

        const auto record = [&](const rhi::GraphicsPipeline& pipeline, const FrameDraw& fd) {
            frame_descriptors_[fd.descriptor_index].bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                         pipeline.layout(), 0);
            TransparentPushConstants p = push;
            p.model = fd.model;
            vkCmdPushConstants(cmd, pipeline.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(p), &p);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &fd.vertex_buffer, &offset);
            vkCmdBindIndexBuffer(cmd, fd.index_buffer, 0, VK_INDEX_TYPE_UINT32);
            for (const asset::SubMesh& sm : fd.submeshes) {
                vkCmdDrawIndexed(cmd, sm.index_count, 1, sm.index_offset, 0, 0);
            }
        };

        // Transmissive glass first: depth-written and culled like opaque
        // geometry, so the blended draws below depth-test against it.
        bool bound = false;
        for (const FrameDraw& fd : frame_draws_) {
            if (!fd.transmissive) continue;
            if (!bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  transmission_pipeline_.handle());
                bound = true;
            }
            vkCmdSetCullMode(cmd, fd.cull_mode); // doubleSided -> CULL_MODE_NONE
            record(transmission_pipeline_, fd);
        }

        // Alpha-blended draws (supplied back-to-front), two-sided, depth read-only.
        bound = false;
        for (const FrameDraw& fd : frame_draws_) {
            if (fd.transmissive) continue;
            if (!bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  blend_pipeline_.handle());
                bound = true;
            }
            record(blend_pipeline_, fd);
        }

        vkCmdEndRendering(cmd);
    });

    return {};
}

} // namespace engine::renderer
