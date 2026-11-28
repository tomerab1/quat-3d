#include "engine/renderer/mesh_pass.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/material_shader_names.hpp"
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

// Fragment entry point of each alpha mode in a GBuffer program.
[[nodiscard]] const char* fragment_entry(GBufferAlphaMode alpha) {
    return alpha == GBufferAlphaMode::masked ? "fragment_masked" : "fragment_main";
}

constexpr std::array<GBufferAlphaMode, 2> kAlphaModes{GBufferAlphaMode::opaque,
                                                      GBufferAlphaMode::masked};

// The GBuffer pipeline for `program` (the built-in gbuffer.spv, one per alpha
// mode, or an authored material's variant, whose only fragment entry point is
// fragment_main — they share this exact layout).
[[nodiscard]] std::expected<rhi::ManagedPipeline, core::Error>
create_gbuffer_pipeline(rhi::PipelineCache& cache, const rhi::LoadedShader& program,
                        VkDescriptorSetLayout set_layout,
                        GBufferAlphaMode alpha = GBufferAlphaMode::opaque) {
    // Vertex input mirrors asset::Vertex.
    const VkVertexInputBindingDescription vbind{0, sizeof(asset::Vertex),
                                                VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 4> vattrs{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(asset::Vertex, uv)},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(asset::Vertex, tangent)},
    }};
    const VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(GBufferPushConstants)};

    rhi::GraphicsPipeline::CreateInfo info{};
    info.vertex = &program;
    info.fragment = &program;
    info.fragment_entry = fragment_entry(alpha);
    info.color_formats = gbuffer_color_formats; // every variant writes every GBuffer target
    info.depth_format = gbuffer_depth_format;
    info.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL; // standard [0,1] depth
    info.cull_mode = VK_CULL_MODE_NONE;                  // double-sided for now
    info.vertex_bindings = {&vbind, 1};
    info.vertex_attributes = vattrs;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    return cache.create_graphics_pipeline(info);
}

// Camera term of the push constants from the view-projection: the eye is the
// point the projection sends to w = 0, i.e. inverse(VP) * (0, 0, 1, 0) in
// homogeneous form. For an orthographic projection that point is at infinity
// (w = 0) and only the direction toward the camera is meaningful.
[[nodiscard]] glm::vec4 camera_from_view_proj(const glm::mat4& view_proj) {
    const glm::vec4 h = glm::inverse(view_proj) * glm::vec4(0.0F, 0.0F, 1.0F, 0.0F);
    if (std::abs(h.w) > 1e-6F * glm::length(glm::vec3(h))) {
        return {glm::vec3(h) / h.w, 1.0F};
    }
    // Orthographic: the view direction is the near->far direction; the camera
    // lies against it.
    const glm::vec4 n = glm::inverse(view_proj) * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
    const glm::vec4 f = glm::inverse(view_proj) * glm::vec4(0.0F, 0.0F, 1.0F, 1.0F);
    const glm::vec3 toward = glm::vec3(n) / n.w - glm::vec3(f) / f.w;
    const float len = glm::length(toward);
    return {len > 0.0F ? toward / len : glm::vec3(0.0F, 0.0F, 1.0F), 0.0F};
}

} // namespace

GBufferVariant gbuffer_variant(const asset::MaterialAsset& material) {
    GBufferVariant v;
    v.module = material.shader; // empty: the built-in program
    v.alpha = (material.params.flags & asset::material_alpha_mask) != 0
                  ? GBufferAlphaMode::masked
                  : GBufferAlphaMode::opaque;
    return v;
}

std::expected<void, core::Error> MeshPass::create_variants(rhi::PipelineCache& cache,
                                                           const std::string& module,
                                                           const rhi::LoadedShader& shader) {
    for (const GBufferAlphaMode alpha : kAlphaModes) {
        auto pipeline = create_gbuffer_pipeline(cache, shader, material_layout_.handle(), alpha);
        if (!pipeline) return std::unexpected(pipeline.error());
        pipelines_.insert_or_assign(GBufferVariant{module, alpha}, std::move(*pipeline));
    }
    return {};
}

float MeshPass::material_time() const {
    if (fixed_time_) return *fixed_time_;
    const std::chrono::duration<double> t = std::chrono::steady_clock::now() - epoch_;
    return static_cast<float>(t.count());
}

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

    if (auto r = out.create_variants(cache, std::string{}, **shader); !r) {
        return std::unexpected(r.error());
    }

    // Authored material shaders: same layout, their own program (A6).
    const VkDescriptorSetLayout set_layout = out.material_layout_.handle();
    out.materials_ = MaterialPipelines(
        cache, cooked_shader_dir, std::string(rhi::material_shaders::variants[0].name), *shader,
        [&cache, set_layout](const rhi::LoadedShader& program) {
            return create_gbuffer_pipeline(cache, program, set_layout);
        });
    out.epoch_ = std::chrono::steady_clock::now();

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

std::expected<void, core::Error> MeshPass::set_texture_lod_bias(float bias) {
    if (bias == lod_bias_ && sampler_.handle() != VK_NULL_HANDLE) return {};
    // The slot's previous frame has finished (frame-slot ownership), and the
    // descriptors are rewritten every add_to_graph: replacing it is safe.
    auto sampler = rhi::create_sampler(device_->handle(), rhi::SamplerAddress::repeat,
                                       kMaterialMaxLod, 0.0F, bias);
    if (!sampler) return std::unexpected(sampler.error());
    sampler_ = std::move(*sampler);
    lod_bias_ = bias;
    return {};
}

std::expected<GBufferTargets, core::Error>
MeshPass::add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                       std::span<const DrawItem> draws) {
    return add_to_graph(graph, extent, view_proj, batch_draws(draws));
}

std::expected<GBufferTargets, core::Error>
MeshPass::add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                       const InstancedDrawList& list) {
    return add_to_graph(graph, extent, view_proj, list, ViewMotion::still(view_proj));
}

std::expected<GBufferTargets, core::Error>
MeshPass::add_to_graph(rhi::RenderGraph& graph, VkExtent2D extent, const glm::mat4& view_proj,
                       const InstancedDrawList& list, const ViewMotion& motion) {
    frame_descriptors_.clear();
    frame_draws_.clear();
    frame_draws_.reserve(list.draws.size());

    if (!list.motion.empty() && list.motion.size() != list.instances.size()) {
        return fail("gbuffer pass: motion records do not match the instance array");
    }
    auto instances = instances_.upload(*device_, *allocator_, list.instances);
    if (!instances) return std::unexpected(instances.error());
    frame_instances_ = *instances;
    auto motion_addr = motion_.upload(*device_, *allocator_, list.motion);
    if (!motion_addr) return std::unexpected(motion_addr.error());
    frame_motion_ = *motion_addr;
    materials_.begin_frame();

    frame_push_ = GBufferPushConstants{};
    frame_push_.view_proj = view_proj;
    frame_push_.instances = frame_instances_;
    frame_push_.time = material_time();
    frame_push_.camera = camera_from_view_proj(view_proj);
    frame_push_.prev_view_proj = motion.prev_view_proj;
    frame_push_.motion = frame_motion_;
    frame_push_.jitter_ndc = motion.jitter_ndc;
    frame_push_.has_motion = frame_motion_ != 0 ? 1U : 0U;

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
    targets.emissive = graph.create_transient_image("gbuffer_emissive", gbuffer_emissive_format,
                                                    extent, color_usage);
    targets.velocity =
        graph.create_transient_image("gbuffer_velocity", velocity_format, extent, color_usage);
    targets.motion = motion;
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
        fd.prev_positions = d.prev_positions;
        fd.first_instance = d.first_instance;
        fd.instance_count = d.instance_count;
        fd.descriptor_index = slot->second;
        const GBufferVariant variant = gbuffer_variant(mat);
        const rhi::ManagedPipeline* custom =
            variant.module.empty() ? nullptr : materials_.find(variant.module);
        if (custom != nullptr) {
            // Authored programs cut out below alpha 0.5 themselves (discard):
            // they draw with the alpha-tested group.
            fd.pipeline = custom;
            fd.alpha = GBufferAlphaMode::masked;
        } else {
            // The built-in program (also for a material shader not compiled yet).
            const auto pipeline = pipelines_.find(GBufferVariant{{}, variant.alpha});
            if (pipeline == pipelines_.end()) {
                return fail("gbuffer pass: no built-in pipeline for the material's alpha mode");
            }
            fd.pipeline = &pipeline->second;
            fd.alpha = variant.alpha;
        }
        frame_draws_.push_back(fd);
    }
    // Opaque variants first, so the alpha-tested ones (no early depth test)
    // are rejected by the depth they wrote; then by pipeline and material to
    // minimise binds.
    std::stable_sort(frame_draws_.begin(), frame_draws_.end(),
                     [](const FrameDraw& l, const FrameDraw& r) {
                         return std::tie(l.alpha, l.pipeline, l.descriptor_index) <
                                std::tie(r.alpha, r.pipeline, r.descriptor_index);
                     });

    const GBufferTargets t = targets;
    graph.add_pass("gbuffer", rhi::PassType::graphics)
        .writes(t.albedo, rhi::ResourceUsage::color_attachment)
        .writes(t.normal, rhi::ResourceUsage::color_attachment)
        .writes(t.material, rhi::ResourceUsage::color_attachment)
        .writes(t.clearcoat, rhi::ResourceUsage::color_attachment)
        .writes(t.emissive, rhi::ResourceUsage::color_attachment)
        .writes(t.velocity, rhi::ResourceUsage::color_attachment)
        .writes(t.depth, rhi::ResourceUsage::depth_attachment)
        .execute([this, t, extent](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            const rhi::ResourceBinding a = ctx.resolve(t.albedo);
            const rhi::ResourceBinding n = ctx.resolve(t.normal);
            const rhi::ResourceBinding m = ctx.resolve(t.material);
            const rhi::ResourceBinding cc = ctx.resolve(t.clearcoat);
            const rhi::ResourceBinding em = ctx.resolve(t.emissive);
            const rhi::ResourceBinding vel = ctx.resolve(t.velocity);
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
            const std::array<VkRenderingAttachmentInfo, gbuffer_color_formats.size()> colors{
                color_attachment(a.view), color_attachment(n.view), color_attachment(m.view),
                color_attachment(cc.view), color_attachment(em.view), color_attachment(vel.view)};

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

            // Every program shares one layout definition; on a program switch
            // re-push the (shared) constants and re-bind the material set.
            // The per-draw range (last frame's skinned positions) is re-pushed
            // when it changes.
            constexpr VkShaderStageFlags k_push_stages =
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            constexpr std::uint32_t k_draw_push_begin =
                offsetof(GBufferPushConstants, has_prev_positions);
            constexpr std::uint32_t k_draw_push_end = offsetof(GBufferPushConstants, camera);
            GBufferPushConstants push = frame_push_;
            const rhi::ManagedPipeline* bound_pipeline = nullptr;
            std::uint32_t bound_descriptor = std::numeric_limits<std::uint32_t>::max();
            for (const FrameDraw& fd : frame_draws_) {
                const bool draw_push_changed = fd.prev_positions != push.prev_positions;
                push.prev_positions = fd.prev_positions;
                push.has_prev_positions = fd.prev_positions != 0 ? 1U : 0U;
                if (fd.pipeline != bound_pipeline) {
                    bound_pipeline = fd.pipeline;
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      bound_pipeline->handle());
                    vkCmdPushConstants(cmd, bound_pipeline->layout(), k_push_stages, 0,
                                       sizeof(push), &push);
                    bound_descriptor = std::numeric_limits<std::uint32_t>::max();
                } else if (draw_push_changed) {
                    vkCmdPushConstants(cmd, bound_pipeline->layout(), k_push_stages,
                                       k_draw_push_begin, k_draw_push_end - k_draw_push_begin,
                                       reinterpret_cast<const std::byte*>(&push) +
                                           k_draw_push_begin);
                }
                if (fd.descriptor_index != bound_descriptor) {
                    frame_descriptors_[fd.descriptor_index].bind(
                        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_pipeline->layout(), 0);
                    bound_descriptor = fd.descriptor_index;
                }

                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &fd.vertex_buffer, &offset);
                vkCmdBindIndexBuffer(cmd, fd.index_buffer, 0, VK_INDEX_TYPE_UINT32);

                // firstInstance selects the draw's instance range; the shader
                // indexes the array with InstanceIndex (which includes it).
                for (const asset::SubMesh& sm : fd.submeshes) {
                    vkCmdDrawIndexed(cmd, sm.index_count, fd.instance_count, sm.index_offset, 0,
                                     fd.first_instance);
                }
            }

            vkCmdEndRendering(cmd);
        });

    return targets;
}

} // namespace engine::renderer
