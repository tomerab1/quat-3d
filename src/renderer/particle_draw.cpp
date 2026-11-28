// ParticlePass draw passes: weighted blended OIT for alpha emitters (accumulate
// + composite) and additive emitters straight into the HDR. See
// particle_pass.hpp.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/device.hpp"
#include "particle_internal.hpp"

namespace engine::renderer {

namespace {

// particle_draw.slang's resources, in ParticlePass's request order.
enum DrawSlot : std::uint8_t { d_frame, d_emitters, d_particles, d_alive, d_counters, d_depth,
                               d_texture,
                               d_sampler, d_cluster_params, d_lights, d_grid, d_indices };
enum CompositeSlot : std::uint8_t { c_accum, c_reveal };

constexpr VkShaderStageFlags kDrawStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr VkDeviceSize kDrawArgsStride = particle_draw_stride * 4;

struct EmitterDraw {
    std::uint32_t emitter = 0;  // index into the frame's emitter records
    std::uint32_t args = 0;     // emitter slot: offset of its draw arguments
    VkImageView   texture = VK_NULL_HANDLE;
};

[[nodiscard]] VkRenderingAttachmentInfo color_attachment(VkImageView view, VkAttachmentLoadOp load,
                                                         VkClearColorValue clear) {
    VkRenderingAttachmentInfo a{};
    a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageView = view;
    a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.loadOp = load;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.clearValue.color = clear;
    return a;
}

void begin(VkCommandBuffer cmd, VkExtent2D extent, std::span<const VkRenderingAttachmentInfo> colors,
           const VkRenderingAttachmentInfo* depth) {
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, extent};
    info.layerCount = 1;
    info.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
    info.pColorAttachments = colors.data();
    info.pDepthAttachment = depth;
    vkCmdBeginRendering(cmd, &info);
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0F;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

// Read-only depth test against the scene (no store: nothing is written).
[[nodiscard]] VkRenderingAttachmentInfo depth_attachment(VkImageView view) {
    VkRenderingAttachmentInfo a{};
    a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    a.imageView = view;
    a.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    return a;
}

} // namespace

std::expected<void, core::Error>
declare_particle_draws(ParticlePass::State& s, ParticleSlot& slot, rhi::RenderGraph& graph,
                       const ParticleFrame& frame, const ParticleTargets& targets,
                       const ParticleDrawContext& context) {
    std::vector<EmitterDraw> alpha;
    std::vector<EmitterDraw> additive;
    for (std::uint32_t i = 0; i < context.emitter_count; ++i) {
        const ParticleEmitterInput& in = frame.emitters[i];
        const EmitterDraw item{i, in.info.slot,
                            in.texture != VK_NULL_HANDLE ? in.texture : s.disc_view.handle()};
        (in.settings->blend == ParticleBlend::alpha ? alpha : additive).push_back(item);
    }
    // One descriptor set per drawn emitter (they differ in the flipbook).
    while (slot.draw_sets.size() < context.emitter_count) {
        auto db = rhi::DescriptorBuffer::create(*s.device, *s.allocator, s.fns, s.draw_layout);
        if (!db) return std::unexpected(db.error());
        slot.draw_sets.push_back(std::move(*db));
    }
    const ClusteredLightBuffers lights =
        targets.lights.valid() ? targets.lights.buffers : s.empty_lights.buffers();
    const auto& b = s.draw_slots;
    const VkDeviceSize pool = s.pool;
    const VkDevice vk = s.device->handle();
    for (std::uint32_t i = 0; i < context.emitter_count; ++i) {
        rhi::DescriptorBuffer& set = slot.draw_sets[i];
        set.write_uniform_buffer(b[d_frame].binding, context.frame_address, sizeof(ParticleFrameGpu));
        set.write_storage_buffer(b[d_emitters].binding, context.emitters_address,
                                 VkDeviceSize{particle_max_emitters} * sizeof(EmitterGpu));
        set.write_storage_buffer(b[d_particles].binding,
                                 rhi::buffer_device_address(vk, s.particles),
                                 pool * sizeof(ParticleGpu));
        set.write_storage_buffer(b[d_alive].binding, rhi::buffer_device_address(vk, s.alive),
                                 pool * 8);
        set.write_storage_buffer(b[d_counters].binding, rhi::buffer_device_address(vk, s.counters),
                                 VkDeviceSize{particle_max_emitters} * particle_counter_stride * 4);
        set.write_sampler(b[d_sampler].binding, s.sampler.handle());
        set.write_uniform_buffer(b[d_cluster_params].binding, lights.params, lights.params_size);
        set.write_storage_buffer(b[d_lights].binding, lights.lights, lights.lights_size);
        set.write_storage_buffer(b[d_grid].binding, lights.grid, lights.grid_size);
        set.write_storage_buffer(b[d_indices].binding, lights.indices, lights.indices_size);
    }
    for (const auto* list : {&alpha, &additive}) {
        for (const EmitterDraw& item : *list) {
            slot.draw_sets[item.emitter].write_sampled_image(
                b[d_texture].binding, item.texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    using U = rhi::ResourceUsage;
    const auto declare_reads = [&](rhi::RenderGraph::PassBuilder& pass) {
        pass.reads(context.particles, U::buffer_read_vertex)
            .reads(context.alive, U::buffer_read_vertex)
            .reads(context.counters, U::buffer_read_vertex)
            .reads(context.draw_args, U::buffer_read_indirect)
            .reads(targets.depth, U::depth_read);
        read_clustered_lights(pass, targets.lights, U::buffer_read_vertex);
    };
    // Records `items` with `pipeline` inside an active rendering scope.
    const auto record = [state = &s, sets = &slot.draw_sets, depth = targets.depth,
                         args = context.draw_args](rhi::PassContext& ctx,
                                                   const rhi::ManagedPipeline& pipeline,
                                                   const std::vector<EmitterDraw>& items) {
        const VkCommandBuffer cmd = ctx.cmd();
        const VkImageView depth_view = ctx.resolve(depth).view;
        const VkBuffer args_buffer = ctx.resolve(args).buffer;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        vkCmdBindIndexBuffer(cmd, state->quad_indices.handle(), 0, VK_INDEX_TYPE_UINT32);
        for (const EmitterDraw& item : items) {
            rhi::DescriptorBuffer& set = (*sets)[item.emitter];
            set.write_sampled_image(state->draw_slots[d_depth].binding, depth_view,
                                    VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
            set.bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0);
            const ParticlePush push{item.emitter, 0, 0, 0};
            vkCmdPushConstants(cmd, pipeline.layout(), kDrawStages, 0, sizeof(push), &push);
            vkCmdDrawIndexedIndirect(cmd, args_buffer, VkDeviceSize{item.args} * kDrawArgsStride,
                                     1, sizeof(VkDrawIndexedIndirectCommand));
        }
    };

    const VkExtent2D extent = frame.extent;
    if (!alpha.empty()) {
        const rhi::ResourceHandle accum = graph.create_transient_image(
            "particle_accum", particle_accum_format, extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        const rhi::ResourceHandle reveal = graph.create_transient_image(
            "particle_reveal", particle_reveal_format, extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        auto wboit = graph.add_pass("particles_wboit", rhi::PassType::graphics);
        declare_reads(wboit);
        wboit.writes(accum, U::color_attachment).writes(reveal, U::color_attachment);
        wboit.execute([&s, record, alpha, accum, reveal, depth = targets.depth,
                       extent](rhi::PassContext& ctx) {
            const std::array<VkRenderingAttachmentInfo, 2> colors{
                color_attachment(ctx.resolve(accum).view, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 {{0.0F, 0.0F, 0.0F, 0.0F}}),
                color_attachment(ctx.resolve(reveal).view, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 {{1.0F, 0.0F, 0.0F, 0.0F}})};
            const VkRenderingAttachmentInfo d = depth_attachment(ctx.resolve(depth).view);
            begin(ctx.cmd(), extent, colors, &d);
            record(ctx, s.draw_wboit, alpha);
            vkCmdEndRendering(ctx.cmd());
        });
        (void)graph.add_pass("particles_composite", rhi::PassType::graphics)
            .reads(accum, U::sampled)
            .reads(reveal, U::sampled)
            .writes(targets.hdr, U::color_attachment)
            .execute([&s, set = &slot.composite_set, accum, reveal, hdr = targets.hdr,
                      extent](rhi::PassContext& ctx) {
                constexpr VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                set->write_sampled_image(s.composite_slots[c_accum].binding,
                                         ctx.resolve(accum).view, ro);
                set->write_sampled_image(s.composite_slots[c_reveal].binding,
                                         ctx.resolve(reveal).view, ro);
                const std::array<VkRenderingAttachmentInfo, 1> colors{color_attachment(
                    ctx.resolve(hdr).view, VK_ATTACHMENT_LOAD_OP_LOAD, {})};
                const VkCommandBuffer cmd = ctx.cmd();
                begin(cmd, extent, colors, nullptr);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.composite.handle());
                set->bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.composite.layout(), 0);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRendering(cmd);
            });
    }
    if (!additive.empty()) {
        auto pass = graph.add_pass("particles_additive", rhi::PassType::graphics);
        declare_reads(pass);
        pass.writes(targets.hdr, U::color_attachment);
        pass.execute([&s, record, additive, hdr = targets.hdr, depth = targets.depth,
                      extent](rhi::PassContext& ctx) {
            const std::array<VkRenderingAttachmentInfo, 1> colors{
                color_attachment(ctx.resolve(hdr).view, VK_ATTACHMENT_LOAD_OP_LOAD, {})};
            const VkRenderingAttachmentInfo d = depth_attachment(ctx.resolve(depth).view);
            begin(ctx.cmd(), extent, colors, &d);
            record(ctx, s.draw_additive, additive);
            vkCmdEndRendering(ctx.cmd());
        });
    }
    return {};
}

} // namespace engine::renderer
