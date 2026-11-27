// Forward transparency and glass transmission renders (moved from main.cpp, S7).

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

// Renders an opaque red wall with a half-transparent blue panel in front and
// verifies the panel pixel is a blend (both red showing through and blue added),
// while a pixel off the panel stays red — proving alpha blending works.
std::expected<void, engine::core::Error>
run_transparent_self_test(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                          engine::rhi::PipelineCache& cache,
                          const engine::rhi::TransferContext& transfer,
                          const std::string& shader_dir) {
    using namespace engine;
    constexpr VkExtent2D extent{128, 128};
    constexpr VkFormat ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    auto wall = scene::upload_mesh(make_box_data(glm::vec3(3.0F, 3.0F, 0.2F)), allocator, transfer);
    auto panel = scene::upload_mesh(make_box_data(glm::vec3(1.0F, 1.0F, 0.05F)), allocator, transfer);
    if (!wall || !panel) return std::unexpected((!wall ? wall : panel).error());

    asset::PbrMaterialParams red_params;
    red_params.base_color_factor = {0.9F, 0.1F, 0.1F, 1.0F};
    red_params.roughness_factor = 0.9F;
    auto red = asset::upload_material(red_params, asset::MaterialTextures{}, allocator, transfer);
    asset::PbrMaterialParams blue_params;
    blue_params.base_color_factor = {0.1F, 0.2F, 0.95F, 0.5F};
    blue_params.roughness_factor = 0.5F;
    blue_params.flags = asset::material_blend;
    auto blue = asset::upload_material(blue_params, asset::MaterialTextures{}, allocator, transfer);
    if (!red || !blue) return std::unexpected((!red ? red : blue).error());

    auto mesh_pass = renderer::MeshPass::create(device, allocator, cache, transfer, shader_dir);
    auto lighting = renderer::LightingPass::create(device, allocator, cache, shader_dir);
    auto transparent =
        renderer::TransparentPass::create(device, allocator, cache, transfer, shader_dir);
    auto tonemap = renderer::TonemapPass::create(device, allocator, cache, shader_dir, ldr_format);
    if (!mesh_pass || !lighting || !transparent || !tonemap) {
        return std::unexpected(core::Error{"transparent self-test: pass creation failed"});
    }

    const glm::vec3 eye(0.0F, 0.0F, 3.5F);
    glm::mat4 proj = glm::perspective(glm::radians(45.0F), 1.0F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F;
    const glm::mat4 view_proj = proj * glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));

    renderer::DirectionalLightParams light;
    light.direction = {0.0F, 0.0F, 1.0F, 0.0F};
    light.color = {1.0F, 1.0F, 1.0F, 3.0F};
    light.ambient = {0.1F, 0.1F, 0.1F, 0.0F};

    const renderer::DrawItem wall_item{&*wall, &*red,
                                       glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -1.0F))};
    const renderer::DrawItem panel_item{&*panel, &*blue, glm::mat4(1.0F)};

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer readback_buffer = readback->handle();

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = mesh_pass->add_to_graph(graph, extent, view_proj, {&wall_item, 1});
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto hdr = lighting->add_to_graph(graph, *gbuffer, extent, light, glm::inverse(view_proj), eye,
                                      rhi::ResourceHandle{}, glm::mat4(1.0F));
    if (!hdr) return std::unexpected(hdr.error());
    if (auto r = transparent->add_to_graph(graph, *hdr, gbuffer->depth, extent, view_proj, light,
                                           eye, {&panel_item, 1});
        !r) {
        return std::unexpected(r.error());
    }
    const rhi::ResourceHandle ldr = graph.create_transient_image(
        "t_ldr", ldr_format, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (auto r = tonemap->add_to_graph(graph, *hdr, ldr, extent); !r) {
        return std::unexpected(r.error());
    }
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    const VkImage ldr_image = graph.binding(ldr).image;

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = device.queue_families().graphics;
    vkCreateCommandPool(device.handle(), &pi, nullptr, &cmd_pool);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
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
    vkCmdCopyImageToBuffer(cmd, ldr_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1,
                           &region);
    vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    ci.commandBuffer = cmd;
    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    const VkResult sr = vkQueueSubmit2(device.graphics_queue(), 1, &si, VK_NULL_HANDLE);
    std::expected<void, core::Error> result{};
    if (sr != VK_SUCCESS) {
        result = std::unexpected(core::Error{"transparent self-test: submit failed"});
    } else {
        vkQueueWaitIdle(device.graphics_queue());
        const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
        const auto chan = [&](std::uint32_t x, std::uint32_t y, int c) {
            return static_cast<int>(px[(static_cast<std::size_t>(y) * extent.width + x) * 4 + c]);
        };
        // Centre is behind the panel: blend of red wall + blue panel -> both present.
        const std::uint32_t cx = extent.width / 2;
        const std::uint32_t cy = extent.height / 2;
        if (chan(cx, cy, 0) <= 25 || chan(cx, cy, 2) <= 25) {
            result = std::unexpected(
                core::Error{"transparent self-test: panel pixel is not a red/blue blend"});
        } else if (chan(6, cy, 0) <= chan(6, cy, 2)) {
            // Off the panel (left edge) the wall is plain red: red > blue.
            result = std::unexpected(
                core::Error{"transparent self-test: off-panel wall is not red"});
        }
    }

    vkFreeCommandBuffers(device.handle(), cmd_pool, 1, &cmd);
    vkDestroyCommandPool(device.handle(), cmd_pool, nullptr);
    return result;
}

// Glass / transmission (7.5): a bright white wall behind a blue-tinted transmission
// panel. The panel must show the wall *through* it, tinted blue — so a pixel behind
// the panel is blue-dominant, while off the panel the wall reads plain white.
[[nodiscard]] std::expected<void, engine::core::Error>
run_glass_self_test(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                    engine::rhi::PipelineCache& cache,
                    const engine::rhi::TransferContext& transfer, const std::string& shader_dir) {
    using namespace engine;
    constexpr VkExtent2D extent{128, 128};
    constexpr VkFormat ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    auto wall = scene::upload_mesh(make_box_data(glm::vec3(3.0F, 3.0F, 0.2F)), allocator, transfer);
    auto panel = scene::upload_mesh(make_box_data(glm::vec3(1.0F, 1.0F, 0.05F)), allocator, transfer);
    if (!wall || !panel) return std::unexpected((!wall ? wall : panel).error());

    asset::PbrMaterialParams white_params;
    white_params.base_color_factor = {1.0F, 1.0F, 1.0F, 1.0F};
    white_params.roughness_factor = 0.9F;
    auto white = asset::upload_material(white_params, asset::MaterialTextures{}, allocator, transfer);
    asset::PbrMaterialParams glass_params;
    glass_params.base_color_factor = {0.1F, 0.25F, 1.0F, 1.0F}; // blue tint
    glass_params.transmission_factor = 1.0F;
    glass_params.roughness_factor = 0.8F; // damp specular
    glass_params.flags = asset::material_transmission;
    auto glass = asset::upload_material(glass_params, asset::MaterialTextures{}, allocator, transfer);
    if (!white || !glass) return std::unexpected((!white ? white : glass).error());

    auto mesh_pass = renderer::MeshPass::create(device, allocator, cache, transfer, shader_dir);
    auto lighting = renderer::LightingPass::create(device, allocator, cache, shader_dir);
    auto transparent =
        renderer::TransparentPass::create(device, allocator, cache, transfer, shader_dir);
    auto tonemap = renderer::TonemapPass::create(device, allocator, cache, shader_dir, ldr_format);
    if (!mesh_pass || !lighting || !transparent || !tonemap) {
        return std::unexpected(core::Error{"glass self-test: pass creation failed"});
    }

    const glm::vec3 eye(0.0F, 0.0F, 3.5F);
    glm::mat4 proj = glm::perspective(glm::radians(45.0F), 1.0F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F;
    const glm::mat4 view_proj = proj * glm::lookAt(eye, glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));

    renderer::DirectionalLightParams light;
    light.direction = {0.0F, 0.0F, 1.0F, 0.0F};
    light.color = {1.0F, 1.0F, 1.0F, 1.5F};
    light.ambient = {0.2F, 0.2F, 0.2F, 0.0F};

    const renderer::DrawItem wall_item{&*wall, &*white,
                                       glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 0.0F, -1.0F))};
    const renderer::DrawItem panel_item{&*panel, &*glass, glm::mat4(1.0F)};

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer readback_buffer = readback->handle();

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = mesh_pass->add_to_graph(graph, extent, view_proj, {&wall_item, 1});
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto hdr = lighting->add_to_graph(graph, *gbuffer, extent, light, glm::inverse(view_proj), eye,
                                      rhi::ResourceHandle{}, glm::mat4(1.0F));
    if (!hdr) return std::unexpected(hdr.error());
    if (auto r = transparent->add_to_graph(graph, *hdr, gbuffer->depth, extent, view_proj, light,
                                           eye, {&panel_item, 1});
        !r) {
        return std::unexpected(r.error());
    }
    const rhi::ResourceHandle ldr = graph.create_transient_image(
        "g_ldr", ldr_format, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (auto r = tonemap->add_to_graph(graph, *hdr, ldr, extent); !r) {
        return std::unexpected(r.error());
    }
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    const VkImage ldr_image = graph.binding(ldr).image;

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = device.queue_families().graphics;
    vkCreateCommandPool(device.handle(), &pi, nullptr, &cmd_pool);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
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
    vkCmdCopyImageToBuffer(cmd, ldr_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1,
                           &region);
    vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    ci.commandBuffer = cmd;
    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &ci;
    const VkResult sr = vkQueueSubmit2(device.graphics_queue(), 1, &si, VK_NULL_HANDLE);
    std::expected<void, core::Error> result{};
    if (sr != VK_SUCCESS) {
        result = std::unexpected(core::Error{"glass self-test: submit failed"});
    } else {
        vkQueueWaitIdle(device.graphics_queue());
        const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
        const auto chan = [&](std::uint32_t x, std::uint32_t y, int c) {
            return static_cast<int>(px[(static_cast<std::size_t>(y) * extent.width + x) * 4 + c]);
        };
        const std::uint32_t cy = extent.height / 2;
        const std::uint32_t px_panel = extent.width / 2 - 18; // behind panel, off the highlight
        const int pr = chan(px_panel, cy, 0);
        const int pb = chan(px_panel, cy, 2);
        const int wr = chan(6, cy, 0);
        const int wb = chan(6, cy, 2);
        if (std::getenv("QUAT_GLASS_DEBUG") != nullptr) {
            std::fprintf(stderr, "[glass] panel rgb=%d,%d,%d  wall rgb=%d,%d,%d\n", pr,
                         chan(px_panel, cy, 1), pb, wr, chan(6, cy, 1), wb);
        }
        if (wr <= 60 || wb <= 60) {
            result = std::unexpected(core::Error{"glass self-test: wall is not lit white"});
        } else if (pb <= 40) {
            result =
                std::unexpected(core::Error{"glass self-test: nothing transmitted through glass"});
        } else if (pb <= pr + 15) {
            result = std::unexpected(
                core::Error{"glass self-test: transmitted wall is not blue-tinted by the glass"});
        }
    }

    vkFreeCommandBuffers(device.handle(), cmd_pool, 1, &cmd);
    vkDestroyCommandPool(device.handle(), cmd_pool, nullptr);
    return result;
}

} // namespace

TEST(GpuRenderScene, AlphaBlendedPanel) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_OK(run_transparent_self_test(g->device, g->allocator, g->cache, g->transfer,
                                        g->shader_dir));
}

TEST(GpuRenderScene, GlassTransmission) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_OK(run_glass_self_test(g->device, g->allocator, g->cache, g->transfer, g->shader_dir));
}
