#include "engine/renderer/mesh_pass.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kMaterialStages = VK_SHADER_STAGE_FRAGMENT_BIT;

constexpr std::uint32_t kMaterialMaxLod = material_sampler_max_lod;

// Material descriptor set 0, matching PbrMaterial in material.slang (verified
// against the gbuffer.slang SPIR-V): uniform params at 0, the five optional
// textures as sampled images at 1..5, and the shared sampler at 6.
[[nodiscard]] std::array<rhi::DescriptorBinding, 7> material_bindings() {
    using rhi::DescriptorBinding;
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kMaterialStages},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kMaterialStages},
        {0, 6, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kMaterialStages},
    }};
}

} // namespace

std::expected<MeshPass, core::Error>
MeshPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                 rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                 const std::string& cooked_shader_dir) {
    MeshPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("mesh pass: descriptor buffer functions unavailable");
    }

    const auto bindings = material_bindings();
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, bindings);
    if (!layout) return std::unexpected(layout.error());
    out.material_layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/gbuffer.spv");
    if (!shader) return std::unexpected(shader.error());

    // Vertex input mirrors asset::Vertex.
    const VkVertexInputBindingDescription vbind{0, sizeof(asset::Vertex),
                                                VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 4> vattrs{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(asset::Vertex, uv)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(asset::Vertex, tangent)},
    }};
    const std::array<VkFormat, 4> color_formats{gbuffer_albedo_format, gbuffer_normal_format,
                                                gbuffer_material_format,
                                                gbuffer_clearcoat_format};
    const VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(GBufferPushConstants)};
    const VkDescriptorSetLayout set_layout = out.material_layout_.handle();

    rhi::GraphicsPipeline::CreateInfo info{};
    info.vertex = *shader;
    info.fragment = *shader;
    info.color_formats = color_formats;
    info.depth_format = gbuffer_depth_format;
    info.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL; // standard [0,1] depth
    info.cull_mode = VK_CULL_MODE_NONE;                  // double-sided for now
    info.vertex_bindings = {&vbind, 1};
    info.vertex_attributes = vattrs;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};

    auto pipeline = cache.create_graphics_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);
    // Same state, alpha-tested fragment shader (glTF MASK).
    info.fragment_entry = "fragment_masked";
    auto masked = cache.create_graphics_pipeline(info);
    if (!masked) return std::unexpected(masked.error());
    out.masked_pipeline_ = std::move(*masked);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::repeat,
                                       kMaterialMaxLod);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    // 1x1 opaque-white fallback for unbound texture slots.
    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    auto img = rhi::upload_device_image(allocator, transfer, white.data(), white.size(),
                                        VkExtent2D{1, 1}, VK_FORMAT_R8G8B8A8_UNORM);
    if (!img) return std::unexpected(img.error());
    auto view = rhi::create_image_view(device.handle(), img->handle(), VK_FORMAT_R8G8B8A8_UNORM);
    if (!view) return std::unexpected(view.error());
    out.fallback_texture_.image = std::move(*img);
    out.fallback_texture_.view = std::move(*view);
    out.fallback_texture_.width = 1;
    out.fallback_texture_.height = 1;
    out.fallback_texture_.format = VK_FORMAT_R8G8B8A8_UNORM;

    auto mat = asset::upload_material(asset::PbrMaterialParams{}, asset::MaterialTextures{},
                                      allocator, transfer);
    if (!mat) return std::unexpected(mat.error());
    out.default_material_ = std::move(*mat);

    return out;
}

VkImageView MeshPass::texture_view_or_fallback(
    const asset::AssetHandle<asset::TextureAsset>& handle) const {
    if (handle.valid() && handle.is_loaded() && handle->valid()) {
        return handle->view.handle();
    }
    return fallback_texture_.view.handle();
}

std::expected<GBufferTargets, core::Error>
MeshPass::add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                       std::span<const DrawItem> draws) {
    return add_to_graph(graph, extent, view_proj, batch_draws(draws));
}

std::expected<GBufferTargets, core::Error>
MeshPass::add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                       const InstancedDrawList& list) {
    frame_descriptors_.clear();
    frame_draws_.clear();
    frame_masked_draws_.clear();
    frame_draws_.reserve(list.draws.size());

    auto instances = instances_.upload(*device_, *allocator_, list.instances);
    if (!instances) return std::unexpected(instances.error());
    frame_instances_ = *instances;

    // GBuffer targets. SAMPLED is added so the lighting pass (3.6) can read them;
    // TRANSFER_SRC lets tests and debug tooling read them back.
    const VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                          | VK_IMAGE_USAGE_SAMPLED_BIT
                                          | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    GBufferTargets targets;
    targets.albedo =
        graph.create_transient_image("gbuffer_albedo", gbuffer_albedo_format, extent, color_usage);
    targets.normal =
        graph.create_transient_image("gbuffer_normal", gbuffer_normal_format, extent, color_usage);
    targets.material = graph.create_transient_image("gbuffer_material", gbuffer_material_format,
                                                    extent, color_usage);
    targets.clearcoat = graph.create_transient_image("gbuffer_clearcoat",
                                                     gbuffer_clearcoat_format, extent, color_usage);
    targets.depth = graph.create_transient_image(
        "gbuffer_depth", gbuffer_depth_format, extent,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT); // TRANSFER_SRC: tests / tools read depth back

    // One descriptor buffer per distinct material this frame (batches already
    // merged identical mesh+material draws; meshes sharing a material share it).
    std::unordered_map<const asset::MaterialAsset*, std::uint32_t> descriptor_of;
    for (const InstancedDraw& d : list.draws) {
        if (d.mesh == nullptr || !d.mesh->valid() || d.instance_count == 0) continue;
        if (static_cast<std::size_t>(d.first_instance) + d.instance_count >
            list.instances.size()) {
            return fail("gbuffer pass: draw instance range exceeds the instance array");
        }
        const asset::MaterialAsset& mat =
            (d.material != nullptr && d.material->valid()) ? *d.material : default_material_;

        auto [slot, inserted] = descriptor_of.try_emplace(
            &mat, static_cast<std::uint32_t>(frame_descriptors_.size()));
        if (inserted) {
            auto db =
                rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, material_layout_);
            if (!db) return std::unexpected(db.error());
            db->write_uniform_buffer(0, mat.param_address, sizeof(asset::PbrMaterialParams));
            constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            db->write_sampled_image(1, texture_view_or_fallback(mat.textures.base_color), ro);
            db->write_sampled_image(2, texture_view_or_fallback(mat.textures.normal), ro);
            db->write_sampled_image(3, texture_view_or_fallback(mat.textures.metallic_roughness),
                                    ro);
            db->write_sampled_image(4, texture_view_or_fallback(mat.textures.emissive), ro);
            db->write_sampled_image(5, texture_view_or_fallback(mat.textures.occlusion), ro);
            db->write_sampler(6, sampler_.handle());
            frame_descriptors_.push_back(std::move(*db));
        }

        FrameDraw fd;
        fd.vertex_buffer = d.skinned_vertices != VK_NULL_HANDLE ? d.skinned_vertices
                                                                : d.mesh->vertex_buffer.handle();
        fd.index_buffer = d.mesh->index_buffer.handle();
        fd.submeshes = d.mesh->submeshes;
        fd.first_instance = d.first_instance;
        fd.instance_count = d.instance_count;
        fd.descriptor_index = slot->second;
        const bool masked = (mat.params.flags & asset::material_alpha_mask) != 0;
        (masked ? frame_masked_draws_ : frame_draws_).push_back(fd);
    }

    const GBufferTargets t = targets;
    const glm::mat4 vp = view_proj;
    graph.add_pass("gbuffer", rhi::PassType::graphics)
        .writes(t.albedo, rhi::ResourceUsage::color_attachment)
        .writes(t.normal, rhi::ResourceUsage::color_attachment)
        .writes(t.material, rhi::ResourceUsage::color_attachment)
        .writes(t.clearcoat, rhi::ResourceUsage::color_attachment)
        .writes(t.depth, rhi::ResourceUsage::depth_attachment)
        .execute([this, t, extent, vp](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            const rhi::ResourceBinding a = ctx.resolve(t.albedo);
            const rhi::ResourceBinding n = ctx.resolve(t.normal);
            const rhi::ResourceBinding m = ctx.resolve(t.material);
            const rhi::ResourceBinding cc = ctx.resolve(t.clearcoat);
            const rhi::ResourceBinding depth = ctx.resolve(t.depth);

            const auto color_attachment = [](VkImageView view) {
                VkRenderingAttachmentInfo ai{};
                ai.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                ai.imageView = view;
                ai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ai.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                ai.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                ai.clearValue.color = {{0.0F, 0.0F, 0.0F, 0.0F}};
                return ai;
            };
            const std::array<VkRenderingAttachmentInfo, 4> colors{
                color_attachment(a.view), color_attachment(n.view), color_attachment(m.view),
                color_attachment(cc.view)};

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
            rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
            rendering.pColorAttachments = colors.data();
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

            // The instance array is shared by every draw: push it once per
            // pipeline. Opaque draws first, so the alpha-tested ones (no
            // early depth test) are rejected by the depth they wrote.
            GBufferPushConstants push{};
            push.view_proj = vp;
            push.instances = frame_instances_;
            const auto draw_all = [&](const rhi::ManagedPipeline& pipeline,
                                      std::span<const FrameDraw> draws) {
                if (draws.empty()) return;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
                vkCmdPushConstants(cmd, pipeline.layout(),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(push), &push);
                std::uint32_t bound_descriptor = std::numeric_limits<std::uint32_t>::max();
                for (const FrameDraw& fd : draws) {
                    if (fd.descriptor_index != bound_descriptor) {
                        frame_descriptors_[fd.descriptor_index].bind(
                            cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0);
                        bound_descriptor = fd.descriptor_index;
                    }

                    const VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &fd.vertex_buffer, &offset);
                    vkCmdBindIndexBuffer(cmd, fd.index_buffer, 0, VK_INDEX_TYPE_UINT32);

                    // firstInstance selects the draw's instance range; the
                    // shader indexes the array with InstanceIndex (which
                    // includes it).
                    for (const asset::SubMesh& sm : fd.submeshes) {
                        vkCmdDrawIndexed(cmd, sm.index_count, fd.instance_count,
                                         sm.index_offset, 0, fd.first_instance);
                    }
                }
            };
            draw_all(pipeline_, frame_draws_);
            draw_all(masked_pipeline_, frame_masked_draws_);

            vkCmdEndRendering(cmd);
        });

    return targets;
}

} // namespace engine::renderer
