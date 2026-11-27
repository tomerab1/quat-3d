#include "engine/renderer/shadow_pass.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/hiz_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr VkDeviceSize       kCascadeBytes = sizeof(GpuCascades);

// Mirrors CsmSetupParams in csm_setup.slang (std140, 176 bytes).
struct CsmSetupParams {
    glm::mat4 inv_view{1.0F};
    glm::mat4 inv_proj{1.0F};
    glm::vec4 to_light{0.0F};
    glm::vec4 split{0.0F}; // count, lambda, max distance, blend band
    glm::vec4 fit{0.0F};   // caster distance, map size, use depth bounds
};
static_assert(sizeof(CsmSetupParams) == 176, "must match csm_setup.slang CsmSetupParams");

constexpr std::array<rhi::BindingRequest, 3> kSetupResources{{
    {"g_params", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_bounds", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_cascades", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
}};

[[nodiscard]] std::expected<rhi::GpuBuffer, core::Error>
mapped_buffer(rhi::GpuAllocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    return allocator.create_buffer(
        size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

} // namespace

std::expected<ShadowPass, core::Error>
ShadowPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                   rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    ShadowPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("shadow pass: descriptor buffer functions unavailable");
    auto empty = rhi::DescriptorSetLayout::create(device, out.db_fns_, {});
    if (!empty) return std::unexpected(empty.error());
    out.empty_layout_ = std::move(*empty);
    const VkDescriptorSetLayout empty_set = out.empty_layout_.handle();

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
    info.set_layouts = {&empty_set, 1};
    auto pipeline = cache.create_graphics_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    // GPU cascade fitting.
    auto setup = cache.load(cooked_shader_dir + "/csm_setup.spv");
    if (!setup) return std::unexpected(setup.error());
    auto bindings = rhi::reflected_bindings((*setup)->reflection, kSetupResources, kComputeStage);
    if (!bindings) return fail("shadow pass: " + bindings.error().message);
    out.setup_slots_ = {(*bindings)[0].binding, (*bindings)[1].binding, (*bindings)[2].binding};
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, *bindings);
    if (!layout) return std::unexpected(layout.error());
    out.setup_layout_ = std::move(*layout);
    const VkDescriptorSetLayout set_layout = out.setup_layout_.handle();
    rhi::ComputePipeline::CreateInfo cinfo{};
    cinfo.shader = *setup;
    cinfo.set_layouts = {&set_layout, 1};
    auto setup_pipeline = cache.create_compute_pipeline(cinfo);
    if (!setup_pipeline) return std::unexpected(setup_pipeline.error());
    out.setup_pipeline_ = std::move(*setup_pipeline);

    auto params = mapped_buffer(allocator, sizeof(CsmSetupParams),
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!params) return std::unexpected(params.error());
    out.setup_params_ = std::move(*params);
    auto cascades = mapped_buffer(allocator, kCascadeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!cascades) return std::unexpected(cascades.error());
    out.cascades_ = std::move(*cascades);
    const VkDevice vk = device.handle();
    out.cascades_address_ = rhi::buffer_device_address(vk, out.cascades_);
    std::memset(out.cascades_.mapped(), 0, kCascadeBytes);

    // The cascaded map: a layered depth image, a 2D-array view for sampling
    // and one single-layer view per cascade to render into (TRANSFER_SRC:
    // tests and tools can read a cascade back).
    auto image = allocator.create_image(
        shadow_map_format, {shadow_map_size, shadow_map_size},
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        1,
        max_cascades);
    if (!image) return std::unexpected(image.error());
    out.map_ = std::move(*image);
    auto array_view = rhi::create_image_view(vk, out.map_.handle(), shadow_map_format,
                                             VK_IMAGE_ASPECT_DEPTH_BIT, 1,
                                             VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, 0, max_cascades);
    if (!array_view) return std::unexpected(array_view.error());
    out.array_view_ = std::move(*array_view);
    for (std::uint32_t c = 0; c < max_cascades; ++c) {
        auto view = rhi::create_image_view(vk, out.map_.handle(), shadow_map_format,
                                           VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_VIEW_TYPE_2D, 0,
                                           c, 1);
        if (!view) return std::unexpected(view.error());
        out.layer_views_.push_back(std::move(*view));
    }
    return out;
}

std::expected<CascadedShadows, core::Error>
ShadowPass::add_to_graph(rhi::RenderGraph& graph, const CsmView& view,
                         const CsmSettings& settings, const HiZTargets* hiz,
                         std::span<const DrawItem> draws) {
    return add_to_graph(graph, view, settings, hiz, batch_draws(draws));
}

std::expected<CascadedShadows, core::Error>
ShadowPass::add_to_graph(rhi::RenderGraph& graph, const CsmView& view,
                         const CsmSettings& settings, const HiZTargets* hiz,
                         const InstancedDrawList& casters,
                         std::span<const ShadowCasterFn> extra_casters) {
    if (device_ == nullptr) return fail("shadow pass: not created");
    const CsmSettings s = settings.sanitized();
    if (!s.enabled) return CascadedShadows{};

    auto instances = instances_.upload(*device_, *allocator_, casters.instances);
    if (!instances) return std::unexpected(instances.error());
    frame_instances_ = *instances;
    frame_draws_.clear();
    frame_draws_.reserve(casters.draws.size());
    for (const InstancedDraw& d : casters.draws) {
        if (d.mesh == nullptr || !d.mesh->valid() || d.instance_count == 0) continue;
        if (static_cast<std::size_t>(d.first_instance) + d.instance_count >
            casters.instances.size()) {
            return fail("shadow pass: draw instance range exceeds the instance array");
        }
        FrameDraw fd;
        fd.vertex_buffer = d.skinned_vertices != VK_NULL_HANDLE ? d.skinned_vertices
                                                                : d.mesh->vertex_buffer.handle();
        fd.index_buffer = d.mesh->index_buffer.handle();
        fd.submeshes = d.mesh->submeshes;
        fd.first_instance = d.first_instance;
        fd.instance_count = d.instance_count;
        frame_draws_.push_back(fd);
    }
    frame_extra_.assign(extra_casters.begin(), extra_casters.end());

    CascadedShadows out;
    out.cascades_address = cascades_address_;
    out.cascades = graph.import_buffer("csm_cascades", cascades_.handle(), kCascadeBytes);
    out.shadow_map = graph.import_image(
        "csm_shadow_map", map_.handle(), array_view_.handle(), shadow_map_format,
        {shadow_map_size, shadow_map_size}, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const bool gpu_fit = s.gpu_depth_bounds && hiz != nullptr && hiz->depth_bounds.valid();
    if (gpu_fit) {
        CsmSetupParams p;
        p.inv_view = glm::inverse(view.view);
        p.inv_proj = glm::inverse(view.projection);
        p.to_light = glm::vec4(glm::normalize(view.to_light), 0.0F);
        p.split = {static_cast<float>(s.cascade_count), s.split_lambda, s.max_distance,
                   s.blend_band};
        p.fit = {s.caster_distance, static_cast<float>(shadow_map_size), 1.0F, 0.0F};
        std::memcpy(setup_params_.mapped(), &p, sizeof(p));
        if (auto f = setup_params_.flush(0, sizeof(p)); !f) return std::unexpected(f.error());
        if (setup_descriptor_.device_address() == 0) {
            auto d = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, setup_layout_);
            if (!d) return std::unexpected(d.error());
            setup_descriptor_ = std::move(*d);
            setup_descriptor_.write_uniform_buffer(
                setup_slots_.params, rhi::buffer_device_address(device_->handle(), setup_params_),
                sizeof(CsmSetupParams));
            setup_descriptor_.write_storage_buffer(setup_slots_.cascades, cascades_address_,
                                                   kCascadeBytes);
        }
        setup_descriptor_.write_storage_buffer(setup_slots_.bounds, hiz->depth_bounds_address,
                                               HiZPass::k_bounds_bytes);
        (void)graph.add_pass("csm_setup", rhi::PassType::compute)
            .reads(hiz->depth_bounds, rhi::ResourceUsage::buffer_read_compute)
            .writes(out.cascades, rhi::ResourceUsage::buffer_write_compute)
            .execute([this](rhi::PassContext& ctx) {
                const VkCommandBuffer cmd = ctx.cmd();
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, setup_pipeline_.handle());
                setup_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                       setup_pipeline_.layout(), 0);
                vkCmdDispatch(cmd, 1, 1, 1);
            });
    } else {
        // CPU fallback: the reference fit over camera near .. max_distance.
        const glm::vec2 range = shadow_range(view.projection, s, false, 0.0F, 0.0F);
        const GpuCascades cpu = fit_cascades(view, s, range, shadow_map_size);
        std::memcpy(cascades_.mapped(), &cpu, sizeof(cpu));
        if (auto f = cascades_.flush(0, sizeof(cpu)); !f) return std::unexpected(f.error());
    }

    const std::uint32_t count = s.cascade_count;
    (void)graph.add_pass("csm_shadow", rhi::PassType::graphics)
        .reads(out.cascades, rhi::ResourceUsage::buffer_read_vertex)
        .writes(out.shadow_map, rhi::ResourceUsage::depth_attachment)
        .execute([this, count](rhi::PassContext& ctx) { record_cascades(ctx.cmd(), count); });
    return out;
}

void ShadowPass::record_cascades(VkCommandBuffer cmd, std::uint32_t count) const {
    const VkExtent2D extent{shadow_map_size, shadow_map_size};
    for (std::uint32_t c = 0; c < count; ++c) {
        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = layer_views_[c].handle();
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attachment.clearValue.depthStencil = {1.0F, 0};

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, extent};
        rendering.layerCount = 1;
        rendering.pDepthAttachment = &depth_attachment;
        vkCmdBeginRendering(cmd, &rendering);

        VkViewport viewport{};
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.maxDepth = 1.0F;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
        ShadowPushConstants push{};
        push.cascades = cascades_address_;
        push.instances = frame_instances_;
        push.cascade = c;
        vkCmdPushConstants(cmd, pipeline_.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push),
                           &push);
        for (const FrameDraw& fd : frame_draws_) {
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &fd.vertex_buffer, &offset);
            vkCmdBindIndexBuffer(cmd, fd.index_buffer, 0, VK_INDEX_TYPE_UINT32);
            // firstInstance selects the instance range (see instancing.slang).
            for (const asset::SubMesh& sm : fd.submeshes) {
                vkCmdDrawIndexed(cmd, sm.index_count, fd.instance_count, sm.index_offset, 0,
                                 fd.first_instance);
            }
        }
        for (const ShadowCasterFn& extra : frame_extra_) {
            if (extra) extra(cmd, c, cascades_address_);
        }
        vkCmdEndRendering(cmd);
    }
}

} // namespace engine::renderer
