#include "engine/renderer/tonemap_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <glm/glm.hpp>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kFragment = VK_SHADER_STAGE_FRAGMENT_BIT;

// Tonemap descriptor set 0: HDR sampled image + sampler + the auto-exposure
// luminance buffer. Matches tonemap.slang.
[[nodiscard]] std::array<rhi::DescriptorBinding, 3> tonemap_bindings() {
    using rhi::DescriptorBinding;
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kFragment},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kFragment},
        {0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
    }};
}

// Mirrors TonemapParams in tonemap.slang.
struct TonemapPushConstants {
    float auto_exposure = 0.0F;
    float key = 0.18F;
    float min_exposure = 0.05F;
    float max_exposure = 16.0F;
    float exposure_scale = 1.0F; // 2^compensation EV
    float pad0 = 0.0F;
    float pad1 = 0.0F;
    float pad2 = 0.0F;
};
static_assert(sizeof(TonemapPushConstants) == 32, "must match tonemap.slang TonemapParams");

} // namespace

std::expected<TonemapPass, core::Error>
TonemapPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                    rhi::PipelineCache& cache, const std::string& cooked_shader_dir,
                    VkFormat output_format) {
    TonemapPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) {
        return fail("tonemap pass: descriptor buffer functions unavailable");
    }

    const auto bindings = tonemap_bindings();
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/tonemap.spv");
    if (!shader) return std::unexpected(shader.error());

    const std::array<VkFormat, 1> color_formats{output_format};
    const VkDescriptorSetLayout set_layout = out.layout_.handle();
    const VkPushConstantRange pc_range{kFragment, 0, sizeof(TonemapPushConstants)};

    rhi::GraphicsPipeline::CreateInfo info{};
    info.vertex = *shader;
    info.fragment = *shader;
    info.color_formats = color_formats; // fullscreen triangle, no vertex input
    info.cull_mode = VK_CULL_MODE_NONE;
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc_range, 1};

    auto pipeline = cache.create_graphics_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    // Fallback exposure buffer (one float) bound when auto-exposure is off.
    auto buf = allocator.create_buffer(
        sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!buf) return std::unexpected(buf.error());
    const float one = 1.0F;
    std::memcpy(buf->mapped(), &one, sizeof(one));
    if (auto flushed = buf->flush(); !flushed) return std::unexpected(flushed.error());
    out.fallback_exposure_buffer_ = std::move(*buf);
    out.fallback_exposure_address_ =
        rhi::buffer_device_address(device.handle(), out.fallback_exposure_buffer_);

    return out;
}

std::expected<void, core::Error>
TonemapPass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr,
                          rhi::ResourceHandle output, VkExtent2D extent,
                          VkDeviceAddress exposure_buffer, const ExposureSettings& exposure) {
    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);

    TonemapPushConstants push{};
    push.auto_exposure = exposure_buffer != 0 && exposure.auto_exposure ? 1.0F : 0.0F;
    push.key = exposure.key;
    push.min_exposure = std::min(exposure.min_exposure, exposure.max_exposure);
    push.max_exposure = std::max(exposure.min_exposure, exposure.max_exposure);
    push.exposure_scale = std::exp2(exposure.compensation_ev);
    const VkDeviceAddress exposure_addr =
        exposure_buffer != 0 ? exposure_buffer : fallback_exposure_address_;

    graph.add_pass("tonemap", rhi::PassType::graphics)
        .reads(hdr, rhi::ResourceUsage::sampled)
        .writes(output, rhi::ResourceUsage::color_attachment)
        .execute([this, hdr, output, extent, push, exposure_addr](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            frame_descriptor_.write_sampled_image(0, ctx.resolve(hdr).view,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            frame_descriptor_.write_sampler(1, sampler_.handle());
            frame_descriptor_.write_storage_buffer(2, exposure_addr, sizeof(float));

            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = ctx.resolve(output).view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.0F, 0.0F, 0.0F, 1.0F}};

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
            viewport.minDepth = 0.0F;
            viewport.maxDepth = 1.0F;
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
            frame_descriptor_.bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0);
            vkCmdPushConstants(cmd, pipeline_.layout(), kFragment, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);

            vkCmdEndRendering(cmd);
        });

    return {};
}

} // namespace engine::renderer
