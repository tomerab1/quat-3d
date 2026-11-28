// ShadowPass: the casters that read their material (R12.11, issues #35 /
// #23) — alpha-tested casters into the depth cascades and translucent
// (transmissive / alpha-blended) casters into the translucency map. See
// shadow_pass.hpp and shadow_material.slang.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kMaterialStages = VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// shadow_material.slang's material resources by name (slots from reflection).
enum MaterialSlot : std::uint8_t { m_params, m_base_color, m_sampler, m_count };
constexpr std::array<rhi::BindingRequest, m_count> kMaterialResources{{
    {"g_params", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_base_color", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_samp", VK_DESCRIPTOR_TYPE_SAMPLER},
}};

// What the previous frame (either slot) may still be doing with the shared
// translucency map: rendering it (colour writes) and the lighting pass
// sampling it (compute).
constexpr VkPipelineStageFlags2 kTranslucentPrevStages =
    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
constexpr VkAccessFlags2 kTranslucentPrevAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

enum class CasterKind : std::uint8_t { opaque, masked, translucent };

[[nodiscard]] CasterKind caster_kind(const asset::MaterialAsset* material) {
    if (material == nullptr || !material->valid()) return CasterKind::opaque;
    const std::uint32_t flags = material->params.flags;
    if ((flags & (asset::material_blend | asset::material_transmission)) != 0) {
        return CasterKind::translucent;
    }
    return (flags & asset::material_alpha_mask) != 0 ? CasterKind::masked : CasterKind::opaque;
}

} // namespace

std::expected<void, core::Error> CsmShared::ensure_translucent(VkDevice device,
                                                               rhi::GpuAllocator& allocator) {
    if (translucent_map_.handle() != VK_NULL_HANDLE) return {};
    auto image = allocator.create_image(
        translucent_shadow_format, {translucent_shadow_map_size, translucent_shadow_map_size},
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, // TRANSFER_SRC: tests read a cascade back
        1, max_cascades);
    if (!image) return std::unexpected(image.error());
    auto array_view = rhi::create_image_view(device, image->handle(), translucent_shadow_format,
                                             VK_IMAGE_ASPECT_COLOR_BIT, 1,
                                             VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 0, max_cascades);
    if (!array_view) return std::unexpected(array_view.error());
    std::vector<rhi::ImageView> layers;
    for (std::uint32_t c = 0; c < max_cascades; ++c) {
        auto view = rhi::create_image_view(device, image->handle(), translucent_shadow_format,
                                           VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_VIEW_TYPE_2D, 0,
                                           c, 1);
        if (!view) return std::unexpected(view.error());
        layers.push_back(std::move(*view));
    }
    translucent_map_ = std::move(*image);
    translucent_array_view_ = std::move(*array_view);
    translucent_layer_views_ = std::move(layers);
    return {};
}

std::expected<void, core::Error>
ShadowPass::create_material_casters(rhi::PipelineCache& cache,
                                    const std::string& cooked_shader_dir) {
    auto shader = cache.load(cooked_shader_dir + "/shadow_material.spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, kMaterialResources,
                                            kMaterialStages);
    if (!bindings) return fail("shadow pass: " + bindings.error().message);
    for (std::size_t i = 0; i < m_count; ++i) material_slots_[i] = (*bindings)[i].binding;
    auto layout = rhi::DescriptorSetLayout::create(*device_, db_fns_, *bindings);
    if (!layout) return std::unexpected(layout.error());
    material_layout_ = std::move(*layout);
    const VkDescriptorSetLayout set_layout = material_layout_.handle();

    const VkVertexInputBindingDescription vbind{0, sizeof(asset::Vertex),
                                                VK_VERTEX_INPUT_RATE_VERTEX};
    // Locations as the vertex entry points declare them.
    const std::array<VkVertexInputAttributeDescription, 2> masked_attrs{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(asset::Vertex, uv)},
    }};
    const std::array<VkVertexInputAttributeDescription, 3> translucent_attrs{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(asset::Vertex, uv)},
    }};
    const VkPushConstantRange pc{kPushStages, 0, sizeof(ShadowMaterialPushConstants)};

    // Alpha-tested casters: the depth cascades' state, plus the material.
    rhi::GraphicsPipeline::CreateInfo info{};
    info.vertex = *shader;
    info.fragment = *shader;
    info.vertex_entry = "vertex_masked";
    info.fragment_entry = "fragment_masked";
    info.depth_format = shared_->format_;
    info.depth_clamp = shared_->pancaked_;
    info.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    info.cull_mode = VK_CULL_MODE_NONE;
    info.vertex_bindings = {&vbind, 1};
    info.vertex_attributes = masked_attrs;
    info.push_constants = {&pc, 1};
    info.set_layouts = {&set_layout, 1};
    auto masked = cache.create_graphics_pipeline(info);
    if (!masked) return std::unexpected(masked.error());
    masked_pipeline_ = std::move(*masked);

    // Translucent casters: colour only, transmittance blending; depth clamp
    // pancakes them like the depth casters (no near-plane clipping).
    const VkFormat color = translucent_shadow_format;
    info.vertex_entry = "vertex_main";
    info.vertex_attributes = translucent_attrs;
    info.fragment_entry = "fragment_translucent";
    info.color_formats = {&color, 1};
    info.depth_format = VK_FORMAT_UNDEFINED;
    info.depth_write = false;
    info.transmittance_blend = true;
    auto translucent = cache.create_graphics_pipeline(info);
    if (!translucent) return std::unexpected(translucent.error());
    translucent_pipeline_ = std::move(*translucent);

    auto sampler = rhi::create_sampler(device_->handle(), rhi::SamplerAddress::repeat,
                                       material_sampler_max_lod);
    if (!sampler) return std::unexpected(sampler.error());
    material_sampler_ = std::move(*sampler);

    auto fallback = allocator_->create_image(
        VK_FORMAT_R8G8B8A8_UNORM, {1, 1},
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!fallback) return std::unexpected(fallback.error());
    fallback_texture_ = std::move(*fallback);
    auto view = rhi::create_image_view(device_->handle(), fallback_texture_.handle(),
                                       VK_FORMAT_R8G8B8A8_UNORM);
    if (!view) return std::unexpected(view.error());
    fallback_view_ = std::move(*view);
    return {};
}

std::expected<void, core::Error> ShadowPass::add_caster(const InstancedDraw& d) {
    FrameDraw fd;
    fd.vertex_buffer = d.skinned_vertices != VK_NULL_HANDLE ? d.skinned_vertices
                                                            : d.mesh->vertex_buffer.handle();
    fd.index_buffer = d.mesh->index_buffer.handle();
    fd.submeshes = d.mesh->submeshes;
    fd.first_instance = d.first_instance;
    fd.instance_count = d.instance_count;
    const CasterKind kind = caster_kind(d.material);
    if (kind == CasterKind::opaque) {
        frame_draws_.push_back(fd);
        return {};
    }
    const asset::MaterialAsset& mat = *d.material;
    auto [slot, inserted] = frame_descriptor_of_.try_emplace(
        &mat, static_cast<std::uint32_t>(frame_descriptors_.size()));
    if (inserted) {
        auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, material_layout_);
        if (!db) return std::unexpected(db.error());
        const bool textured = mat.textures.base_color.valid() &&
                              mat.textures.base_color.is_loaded() &&
                              mat.textures.base_color->valid();
        db->write_uniform_buffer(material_slots_[m_params], mat.param_address,
                                 sizeof(asset::PbrMaterialParams));
        db->write_sampled_image(material_slots_[m_base_color],
                                textured ? mat.textures.base_color->view.handle()
                                         : fallback_view_.handle(),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        db->write_sampler(material_slots_[m_sampler], material_sampler_.handle());
        frame_descriptors_.push_back(std::move(*db));
    }
    fd.descriptor = slot->second;
    (kind == CasterKind::masked ? frame_masked_ : frame_translucent_).push_back(fd);
    return {};
}

rhi::ResourceHandle ShadowPass::add_fallback_clear(rhi::RenderGraph& graph) {
    if (fallback_ready_ || (frame_masked_.empty() && frame_translucent_.empty())) return {};
    fallback_ready_ = true;
    const rhi::ResourceHandle fallback = graph.import_image(
        "shadow_material_fallback", fallback_texture_.handle(), fallback_view_.handle(),
        VK_FORMAT_R8G8B8A8_UNORM, {1, 1}, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    (void)graph.add_pass("shadow_material_fallback", rhi::PassType::transfer)
        .writes(fallback, rhi::ResourceUsage::transfer_dst)
        .execute([fallback](rhi::PassContext& ctx) {
            const VkClearColorValue white{{1.0F, 1.0F, 1.0F, 1.0F}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(ctx.cmd(), ctx.resolve(fallback).image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        });
    return fallback;
}

std::expected<void, core::Error>
ShadowPass::add_translucent_pass(rhi::RenderGraph& graph, CascadedShadows& out,
                                 std::uint32_t count, rhi::ResourceHandle fallback) {
    if (frame_translucent_.empty()) return {};
    if (auto r = shared_->ensure_translucent(device_->handle(), *allocator_); !r) return r;
    // Shared by the frame slots like the depth map: discard the old contents
    // once the previous frame's writes and lighting reads are done.
    out.transmittance = graph.import_image(
        "csm_transmittance", shared_->translucent_map_.handle(),
        shared_->translucent_array_view_.handle(), translucent_shadow_format,
        {translucent_shadow_map_size, translucent_shadow_map_size}, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_UNDEFINED, kTranslucentPrevStages, kTranslucentPrevAccess);
    auto pass = graph.add_pass("csm_translucent", rhi::PassType::graphics);
    pass.reads(out.cascades, rhi::ResourceUsage::buffer_read_vertex)
        .writes(out.transmittance, rhi::ResourceUsage::color_attachment);
    if (fallback.valid()) pass.reads(fallback, rhi::ResourceUsage::sampled);
    pass.execute([this, count](rhi::PassContext& ctx) { record_translucent(ctx.cmd(), count); });
    return {};
}

void ShadowPass::draw_material_casters(VkCommandBuffer cmd, const rhi::ManagedPipeline& pipeline,
                                       std::span<const FrameDraw> draws,
                                       std::uint32_t cascade) const {
    if (draws.empty()) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    ShadowMaterialPushConstants push{};
    push.cascades = cascades_address_;
    push.instances = frame_instances_;
    push.to_light = frame_to_light_;
    push.cascade = cascade;
    vkCmdPushConstants(cmd, pipeline.layout(), kPushStages, 0, sizeof(push), &push);
    std::uint32_t bound = std::numeric_limits<std::uint32_t>::max();
    for (const FrameDraw& fd : draws) {
        if (fd.descriptor != bound) {
            frame_descriptors_[fd.descriptor].bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                   pipeline.layout(), 0);
            bound = fd.descriptor;
        }
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &fd.vertex_buffer, &offset);
        vkCmdBindIndexBuffer(cmd, fd.index_buffer, 0, VK_INDEX_TYPE_UINT32);
        // firstInstance selects the instance range (see instancing.slang).
        for (const asset::SubMesh& sm : fd.submeshes) {
            vkCmdDrawIndexed(cmd, sm.index_count, fd.instance_count, sm.index_offset, 0,
                             fd.first_instance);
        }
    }
}

void ShadowPass::record_translucent(VkCommandBuffer cmd, std::uint32_t count) const {
    const VkExtent2D extent{translucent_shadow_map_size, translucent_shadow_map_size};
    for (std::uint32_t c = 0; c < count; ++c) {
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = shared_->translucent_layer_views_[c].handle();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        // Nothing in the way: all light through, no caster (depth 1).
        color.clearValue.color = {{1.0F, 1.0F, 1.0F, 1.0F}};

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);

        VkViewport viewport{};
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        draw_material_casters(cmd, translucent_pipeline_, frame_translucent_, c);
        vkCmdEndRendering(cmd);
    }
}

} // namespace engine::renderer
