// Full deferred chain renders: camera framing and shadows (moved from main.cpp, S7).

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

// Offscreen render of the demo scene (cube + perspective camera + sun) through
// the full deferred chain into an LDR image, read back to verify the camera
// frames the cube: the centre pixel (on the cube) is lit and a corner pixel
// (background) stays black. Unlike the Phase 3 chain self-tests this drives the
// real camera_system matrices, so it catches a broken projection / view / Y-flip
// that "runs clean" would miss.
std::expected<void, engine::core::Error>
run_scene_render_self_test(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                           engine::rhi::PipelineCache& cache,
                           const engine::rhi::TransferContext& transfer,
                           const std::string& shader_dir) {
    using namespace engine;
    constexpr VkExtent2D extent{128, 128};
    constexpr VkFormat ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    auto mesh = scene::upload_mesh(make_cube_data(), allocator, transfer);
    if (!mesh) return std::unexpected(mesh.error());
    asset::PbrMaterialParams params;
    params.base_color_factor = {0.85F, 0.35F, 0.2F, 1.0F};
    params.metallic_factor = 0.0F;
    params.roughness_factor = 0.6F;
    auto material = asset::upload_material(params, asset::MaterialTextures{}, allocator, transfer);
    if (!material) return std::unexpected(material.error());

    auto mesh_pass = renderer::MeshPass::create(device, allocator, cache, transfer, shader_dir);
    if (!mesh_pass) return std::unexpected(mesh_pass.error());
    auto lighting_pass = renderer::LightingPass::create(device, allocator, cache, shader_dir);
    if (!lighting_pass) return std::unexpected(lighting_pass.error());
    auto tonemap_pass =
        renderer::TonemapPass::create(device, allocator, cache, shader_dir, ldr_format);
    if (!tonemap_pass) return std::unexpected(tonemap_pass.error());

    // Camera framing the cube; aspect 1 for the square render target.
    scene::Scene s;
    const entt::entity cam_e = s.create_entity("camera");
    s.registry().get<scene::Transform>(cam_e).local =
        glm::inverse(glm::lookAt(glm::vec3(3.0F, 2.5F, 4.0F), glm::vec3(0.0F),
                                 glm::vec3(0.0F, 1.0F, 0.0F)));
    s.registry().emplace<scene::Camera>(cam_e);
    s.tick();
    const scene::CameraMatrices cam = scene::camera_system(s.registry(), 1.0F);

    renderer::DirectionalLightParams light;
    light.direction = glm::vec4(-glm::normalize(glm::vec3(-0.4F, -1.0F, -0.3F)), 0.0F);
    light.color = glm::vec4(1.0F, 0.98F, 0.95F, 3.0F);

    const renderer::DrawItem item{&*mesh, &*material, glm::mat4(1.0F)};

    auto readback = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer readback_buffer = readback->handle();

    rhi::TransientImagePool pool(device, allocator);
    rhi::RenderGraph graph(pool);
    auto gbuffer = mesh_pass->add_to_graph(graph, extent, cam.view_proj, {&item, 1});
    if (!gbuffer) return std::unexpected(gbuffer.error());
    auto hdr = lighting_pass->add_to_graph(graph, *gbuffer, extent, light,
                                           glm::inverse(cam.view_proj), cam.position,
                                           {}, {},
                                           /*enable_sky=*/true);
    if (!hdr) return std::unexpected(hdr.error());
    const rhi::ResourceHandle ldr = graph.create_transient_image(
        "scene_ldr", ldr_format, extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (auto r = tonemap_pass->add_to_graph(graph, *hdr, ldr, extent); !r) {
        return std::unexpected(r.error());
    }
    if (auto compiled = graph.compile(); !compiled) return std::unexpected(compiled.error());

    const VkImage ldr_image = graph.binding(ldr).image;

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

    // Tonemap left the LDR image in COLOR_ATTACHMENT_OPTIMAL; move it to
    // TRANSFER_SRC for the read-back copy.
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

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;

    std::expected<void, core::Error> result{};
    if (vkQueueSubmit2(device.graphics_queue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        result = std::unexpected(core::Error{"scene render self-test: vkQueueSubmit2 failed"});
    } else {
        vkQueueWaitIdle(device.graphics_queue());
        const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
        const auto chan = [&](std::uint32_t x, std::uint32_t y, int c) {
            return static_cast<int>(px[(static_cast<std::size_t>(y) * extent.width + x) * 4 + c]);
        };
        const auto luma = [&](std::uint32_t x, std::uint32_t y) {
            return chan(x, y, 0) + chan(x, y, 1) + chan(x, y, 2);
        };
        if (luma(extent.width / 2, extent.height / 2) <= 30) {
            result = std::unexpected(
                core::Error{"scene render self-test: cube centre is black (camera not framing it)"});
        } else if (luma(2, 2) <= 60 || chan(2, 2, 2) <= chan(2, 2, 0)) {
            // The corner is sky now: it must be bright and blue-dominant.
            result = std::unexpected(
                core::Error{"scene render self-test: sky background missing or not blue"});
        }
    }

    vkFreeCommandBuffers(device.handle(), cmd_pool, 1, &cmd);
    vkDestroyCommandPool(device.handle(), cmd_pool, nullptr);
    return result;
}

// Renders a box hovering over a floor, once with shadows and once without, and
// verifies the shadowed image is darker overall — i.e. the shadow map actually
// removes light from the floor (a no-op shadow would leave the two identical).
std::expected<void, engine::core::Error>
run_shadow_self_test(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                     engine::rhi::PipelineCache& cache,
                     const engine::rhi::TransferContext& transfer, const std::string& shader_dir) {
    using namespace engine;
    constexpr VkExtent2D extent{128, 128};
    constexpr VkFormat ldr_format = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

    auto floor = scene::upload_mesh(make_box_data(glm::vec3(4.0F, 0.25F, 4.0F)), allocator, transfer);
    auto block = scene::upload_mesh(make_box_data(glm::vec3(0.7F)), allocator, transfer);
    if (!floor || !block) return std::unexpected((!floor ? floor : block).error());
    asset::PbrMaterialParams params;
    params.base_color_factor = {0.8F, 0.8F, 0.8F, 1.0F};
    params.metallic_factor = 0.0F;
    params.roughness_factor = 0.9F;
    auto material = asset::upload_material(params, asset::MaterialTextures{}, allocator, transfer);
    if (!material) return std::unexpected(material.error());

    auto mesh_pass = renderer::MeshPass::create(device, allocator, cache, transfer, shader_dir);
    auto lighting = renderer::LightingPass::create(device, allocator, cache, shader_dir);
    auto shadow_pass = renderer::ShadowPass::create(device, allocator, cache, shader_dir);
    auto tonemap = renderer::TonemapPass::create(device, allocator, cache, shader_dir, ldr_format);
    if (!mesh_pass || !lighting || !shadow_pass || !tonemap) {
        return std::unexpected(core::Error{"shadow self-test: pass creation failed"});
    }

    // Angled camera looking down at the floor; an overhead light tilted slightly.
    const glm::vec3 eye(4.0F, 5.0F, 5.0F);
    glm::mat4 proj = glm::perspective(glm::radians(50.0F), 1.0F, 0.1F, 100.0F);
    proj[1][1] *= -1.0F;
    const glm::mat4 view =
        glm::lookAt(eye, glm::vec3(0.0F, 0.5F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::mat4 view_proj = proj * view;

    const glm::vec3 travel = glm::normalize(glm::vec3(0.15F, -1.0F, 0.1F));
    renderer::DirectionalLightParams light;
    light.direction = glm::vec4(-travel, 0.0F);
    light.color = glm::vec4(1.0F, 1.0F, 1.0F, 3.0F);
    light.ambient = glm::vec4(0.05F, 0.05F, 0.05F, 0.0F);

    // Cascaded shadows fitted on the CPU (no Hi-Z here).
    const renderer::CsmView csm_view{view, proj, -travel};
    renderer::CsmSettings csm;
    csm.max_distance = 30.0F;

    const std::array<renderer::DrawItem, 2> draws{
        renderer::DrawItem{&*floor, &*material, glm::mat4(1.0F)},
        renderer::DrawItem{&*block, &*material,
                           glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 1.6F, 0.0F))}};

    const auto render_luma = [&](bool use_shadow) -> std::expected<long long, core::Error> {
        auto readback = allocator.create_buffer(
            bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!readback) return std::unexpected(readback.error());
        const VkBuffer readback_buffer = readback->handle();

        rhi::TransientImagePool pool(device, allocator);
        rhi::RenderGraph graph(pool);
        renderer::LightingOcclusion occlusion;
        occlusion.csm = csm;
        if (use_shadow) {
            auto s = shadow_pass->add_to_graph(graph, csm_view, csm, nullptr, draws);
            if (!s) return std::unexpected(s.error());
            occlusion.shadows = *s;
        }
        auto gbuffer = mesh_pass->add_to_graph(graph, extent, view_proj, draws);
        if (!gbuffer) return std::unexpected(gbuffer.error());
        auto hdr = lighting->add_to_graph(graph, *gbuffer, extent, light, glm::inverse(view_proj),
                                          eye, occlusion);
        if (!hdr) return std::unexpected(hdr.error());
        const rhi::ResourceHandle ldr = graph.create_transient_image(
            "shadow_ldr", ldr_format, extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (auto r = tonemap->add_to_graph(graph, *hdr, ldr, extent); !r) {
            return std::unexpected(r.error());
        }
        if (auto c = graph.compile(); !c) return std::unexpected(c.error());
        const VkImage ldr_image = graph.binding(ldr).image;

        VkCommandPool pool_h = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = device.queue_families().graphics;
        vkCreateCommandPool(device.handle(), &pi, nullptr, &pool_h);
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool_h;
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
        vkCmdCopyImageToBuffer(cmd, ldr_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_buffer, 1, &region);
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        ci.commandBuffer = cmd;
        VkSubmitInfo2 si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &ci;
        const VkResult sr = vkQueueSubmit2(device.graphics_queue(), 1, &si, VK_NULL_HANDLE);
        long long sum = 0;
        if (sr == VK_SUCCESS) {
            vkQueueWaitIdle(device.graphics_queue());
            const auto* px = static_cast<const std::uint8_t*>(readback->mapped());
            for (VkDeviceSize i = 0; i < bytes; i += 4) {
                sum += static_cast<long long>(px[i]) + px[i + 1] + px[i + 2];
            }
        }
        vkDestroyCommandPool(device.handle(), pool_h, nullptr);
        if (sr != VK_SUCCESS) return std::unexpected(core::Error{"shadow self-test: submit failed"});
        return sum;
    };

    auto unshadowed = render_luma(false);
    if (!unshadowed) return std::unexpected(unshadowed.error());
    auto shadowed = render_luma(true);
    if (!shadowed) return std::unexpected(shadowed.error());

    // Shadows can only remove light, so an effective shadow map makes the floor
    // strictly darker; a no-op shadow would leave the two images identical.
    if (*shadowed >= *unshadowed) {
        return std::unexpected(core::Error{"shadow self-test: shadows did not darken the scene"});
    }
    return {};
}

} // namespace

TEST(GpuRenderScene, CameraFramesTheScene) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_OK(run_scene_render_self_test(g->device, g->allocator, g->cache, g->transfer,
                                         g->shader_dir));
}

TEST(GpuRenderScene, ShadowsDarkenTheFloor) {
    QUAT_REQUIRE_GPU(g);
    ASSERT_OK(run_shadow_self_test(g->device, g->allocator, g->cache, g->transfer, g->shader_dir));
}
