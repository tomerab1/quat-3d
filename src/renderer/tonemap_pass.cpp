#include "engine/renderer/tonemap_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include <glm/glm.hpp>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kFragment = VK_SHADER_STAGE_FRAGMENT_BIT;

// Tonemap descriptor set 0: HDR sampled image + sampler. Matches tonemap.slang.
[[nodiscard]] std::array<rhi::DescriptorBinding, 2> tonemap_bindings() {
    using rhi::DescriptorBinding;
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kFragment},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kFragment},
    }};
}

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

    rhi::GraphicsPipeline::CreateInfo info{};
    info.vertex = *shader;
    info.fragment = *shader;
    info.color_formats = color_formats; // fullscreen triangle, no vertex input
    info.cull_mode = VK_CULL_MODE_NONE;
    info.set_layouts = {&set_layout, 1};

    auto pipeline = rhi::GraphicsPipeline::create(device, cache.handle(), info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    return out;
}

std::expected<void, core::Error>
TonemapPass::add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr,
                          rhi::ResourceHandle output, VkExtent2D extent) {
    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    frame_descriptor_ = std::move(*db);

    graph.add_pass("tonemap", rhi::PassType::graphics)
        .reads(hdr, rhi::ResourceUsage::sampled)
        .writes(output, rhi::ResourceUsage::color_attachment)
        .execute([this, hdr, output, extent](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            frame_descriptor_.write_sampled_image(0, ctx.resolve(hdr).view,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            frame_descriptor_.write_sampler(1, sampler_.handle());

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
            vkCmdDraw(cmd, 3, 1, 0, 0);

            vkCmdEndRendering(cmd);
        });

    return {};
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] glm::vec3 aces(glm::vec3 x) {
    const float a = 2.51F, b = 0.03F, c = 2.43F, d = 0.59F, e = 0.14F;
    glm::vec3 num = x * (a * x + b);
    glm::vec3 den = x * (c * x + d) + e;
    return glm::clamp(num / den, glm::vec3(0.0F), glm::vec3(1.0F));
}

[[nodiscard]] float linear_to_srgb(float c) {
    return c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(std::max(c, 0.0F), 1.0F / 2.4F) - 0.055F;
}

template <typename Record>
[[nodiscard]] std::expected<void, core::Error>
run_graphics_commands(const rhi::Device& device, Record&& record) {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return fail("tonemap self-test: vkCreateCommandPool failed");
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    record(cmd);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    const VkResult r = vkQueueSubmit2(device.graphics_queue(), 1, &submit, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) vkQueueWaitIdle(device.graphics_queue());
    vkDestroyCommandPool(device.handle(), pool, nullptr);
    return r == VK_SUCCESS ? std::expected<void, core::Error>{}
                           : fail("tonemap self-test: vkQueueSubmit2 failed");
}

} // namespace

std::expected<void, core::Error>
run_tonemap_pass_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                           rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                           const std::string& cooked_shader_dir) {
    constexpr VkExtent2D extent{64, 64};
    constexpr VkFormat ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    std::array<asset::Vertex, 3> verts{};
    verts[0].position = {-0.5F, -0.5F, 0.0F};
    verts[1].position = {0.5F, -0.5F, 0.0F};
    verts[2].position = {0.0F, 0.5F, 0.0F};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};

    auto vbuf = rhi::upload_device_buffer(allocator, transfer, verts.data(),
                                          verts.size() * sizeof(asset::Vertex),
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vbuf) return std::unexpected(vbuf.error());
    auto ibuf = rhi::upload_device_buffer(allocator, transfer, indices.data(),
                                          indices.size() * sizeof(std::uint32_t),
                                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!ibuf) return std::unexpected(ibuf.error());

    asset::MeshAsset mesh;
    mesh.vertex_buffer = std::move(*vbuf);
    mesh.index_buffer = std::move(*ibuf);
    mesh.vertex_count = 3;
    mesh.index_count = 3;
    mesh.submeshes = {asset::SubMesh{0, 3, 0}};

    const glm::vec3 base{0.25F, 0.5F, 0.75F};
    asset::PbrMaterialParams params;
    params.base_color_factor = {base, 1.0F};
    params.metallic_factor = 0.0F;
    auto material = asset::upload_material(params, asset::MaterialTextures{}, allocator, transfer);
    if (!material) return std::unexpected(material.error());

    auto mesh_pass = MeshPass::create(device, allocator, cache, transfer, cooked_shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    auto lighting = LightingPass::create(device, allocator, cache, cooked_shader_dir);
    if (!lighting) return std::unexpected(lighting.error());
    auto tonemap = TonemapPass::create(device, allocator, cache, cooked_shader_dir, ldr_format);
    if (!tonemap) return std::unexpected(tonemap.error());

    DirectionalLightParams light;
    light.direction = {0.0F, 0.0F, 1.0F, 0.0F};
    light.color = {1.0F, 1.0F, 1.0F, 1.0F};
    light.ambient = {0.0F, 0.0F, 0.0F, 0.0F};

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer readback_buffer = readback->handle();

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    const DrawItem item{&mesh, &*material, glm::mat4(1.0F)};
    auto targets = mesh_pass->add_to_graph(graph, extent, glm::mat4(1.0F), {&item, 1});
    if (!targets) return std::unexpected(targets.error());
    auto hdr = lighting->add_to_graph(graph, *targets, extent, light);
    if (!hdr) return std::unexpected(hdr.error());
    const rhi::ResourceHandle ldr = graph.create_transient_image(
        "ldr_color", ldr_format, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (auto added = tonemap->add_to_graph(graph, *hdr, ldr, extent); !added) {
        return std::unexpected(added.error());
    }
    if (auto compiled = graph.compile(); !compiled) return std::unexpected(compiled.error());

    const VkImage ldr_image = graph.binding(ldr).image;
    auto submitted = run_graphics_commands(device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);

        VkImageMemoryBarrier2 to_src{};
        to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_src.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_src.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_src.image = ldr_image;
        to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &to_src;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd, ldr_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_buffer, 1, &region);
    });
    if (!submitted) return std::unexpected(submitted.error());

    // Expected LDR = sRGB(ACES(lit)). The GBuffer quantised the base colour to 8
    // bits, so use the same quantised value the lighting pass actually read.
    glm::vec3 quantised;
    for (int i = 0; i < 3; ++i) {
        quantised[i] = std::round(base[i] * 255.0F) / 255.0F;
    }
    const glm::vec3 mapped = aces(quantised);
    const std::array<int, 3> expected{
        static_cast<int>(std::round(linear_to_srgb(mapped.x) * 255.0F)),
        static_cast<int>(std::round(linear_to_srgb(mapped.y) * 255.0F)),
        static_cast<int>(std::round(linear_to_srgb(mapped.z) * 255.0F))};

    const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
        return px + (static_cast<std::size_t>(y) * extent.width + x) * 4;
    };
    const auto near = [](std::uint8_t v, int target) { return std::abs(int(v) - target) <= 2; };

    const std::uint8_t* c = pixel(extent.width / 2, extent.height / 2);
    if (!near(c[0], expected[0]) || !near(c[1], expected[1]) || !near(c[2], expected[2])) {
        return fail("tonemap self-test: centre pixel is not the expected ACES+sRGB colour");
    }
    // Background HDR is 0 -> ACES(0)=0 -> sRGB(0)=0.
    const std::uint8_t* corner = pixel(1, 1);
    if (corner[0] != 0 || corner[1] != 0 || corner[2] != 0) {
        return fail("tonemap self-test: corner pixel is not black");
    }
    return {};
}

} // namespace engine::renderer
