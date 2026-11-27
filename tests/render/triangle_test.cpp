// First-triangle render through the render graph (moved from main.cpp, S7).

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/renderer/transparent_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/primitives.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

using engine::scene::make_box_data;
using engine::scene::make_cube_data;
using engine::scene::make_sphere_data;

// Records the triangle pass into `cmd`: dynamic-rendering scope clearing the
// colour attachment to black, then a 3-vertex draw of the hard-coded triangle.
// The image is already in COLOR_ATTACHMENT_OPTIMAL (the render graph inserted
// the barrier) and is transitioned to PRESENT_SRC by the graph afterwards.
void record_triangle(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent,
                     VkPipeline pipeline) {
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = view;
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

    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

// Offscreen verification of the triangle (display-independent). Renders the
// triangle into a transient colour image via the render graph, copies it to a
// host buffer, and checks the centre pixel (inside the triangle) is non-black
// while a corner (outside it) stays at the clear colour. Confirms the pipeline,
// dynamic rendering, and barrier insertion actually produce pixels.
std::expected<void, engine::core::Error>
run_triangle_self_test(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                       const std::string& mesh_spv) {
    using engine::core::Error;
    constexpr VkExtent2D extent{64, 64};
    constexpr VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    auto cache = engine::rhi::PipelineCache::create(device);
    if (!cache) return std::unexpected(cache.error());
    auto shader = cache->load(mesh_spv);
    if (!shader) return std::unexpected(shader.error());

    auto pipeline = engine::rhi::GraphicsPipeline::create(
        device, cache->handle(),
        {.vertex = *shader, .fragment = *shader, .color_formats = {&fmt, 1}});
    if (!pipeline) return std::unexpected(pipeline.error());

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer readback_buffer = readback->handle();
    const VkPipeline vk_pipeline = pipeline->handle();

    engine::rhi::TransientImagePool pool(device, allocator);
    engine::rhi::RenderGraph graph(pool);
    const engine::rhi::ResourceHandle color = graph.create_transient_image(
        "selftest_color", fmt, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    graph.add_pass("draw", engine::rhi::PassType::graphics)
        .writes(color, engine::rhi::ResourceUsage::color_attachment)
        .execute([=](engine::rhi::PassContext& ctx) {
            const auto b = ctx.resolve(color);
            record_triangle(ctx.cmd(), b.view, extent, vk_pipeline);
        });
    graph.add_pass("copy", engine::rhi::PassType::transfer)
        .reads(color, engine::rhi::ResourceUsage::transfer_src)
        .execute([=](engine::rhi::PassContext& ctx) {
            const auto b = ctx.resolve(color);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(ctx.cmd(), b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback_buffer, 1, &region);
        });
    if (auto r = graph.compile(); !r) return std::unexpected(r.error());

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    vkCreateCommandPool(device.handle(), &pool_info, nullptr, &cmd_pool);

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = cmd_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    graph.execute(cmd);
    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;

    std::expected<void, Error> result{};
    if (vkQueueSubmit2(device.graphics_queue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        result = std::unexpected(Error{"triangle self-test: vkQueueSubmit2 failed"});
    } else {
        vkQueueWaitIdle(device.graphics_queue());
        std::vector<std::uint8_t> pixels(bytes);
        std::memcpy(pixels.data(), readback->mapped(), bytes);
        auto luma = [&](std::uint32_t x, std::uint32_t y) {
            const std::size_t o = (static_cast<std::size_t>(y) * extent.width + x) * 4;
            return static_cast<int>(pixels[o]) + pixels[o + 1] + pixels[o + 2];
        };
        const int centre = luma(extent.width / 2, extent.height / 2);
        const int corner = luma(0, 0);
        if (centre <= 30) {
            result = std::unexpected(Error{"triangle self-test: centre pixel is black (no triangle)"});
        } else if (corner > 30) {
            result = std::unexpected(Error{"triangle self-test: corner pixel not clear-black"});
        }
    }

    vkFreeCommandBuffers(device.handle(), cmd_pool, 1, &cmd);
    vkDestroyCommandPool(device.handle(), cmd_pool, nullptr);
    return result;
}

} // namespace

TEST(GpuRenderScene, Triangle) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_OK(run_triangle_self_test(g->device, g->allocator, g->shader_dir + "/mesh.spv"));
}
