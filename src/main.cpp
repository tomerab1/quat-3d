// quat-3d engine entry point.
//
// Phase 2, Slice 2.5: a hard-coded triangle is drawn via the render graph. Each
// frame builds a RenderGraph that imports the acquired swapchain image and runs
// a single GraphicsPass (dynamic rendering) that clears to black and draws the
// triangle from mesh.slang. The graph inserts the layout barriers and the
// trailing transition to PRESENT_SRC. Two frames in flight.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <limits>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/rhi/swapchain.hpp"

namespace {

constexpr int  k_window_width  = 1280;
constexpr int  k_window_height = 720;
constexpr char k_window_title[] = "quat-3d";
constexpr std::uint32_t k_frames_in_flight = 2;

#ifndef NDEBUG
constexpr bool k_enable_validation = true;
#else
constexpr bool k_enable_validation = false;
#endif

VkExtent2D window_pixel_extent(SDL_Window* window) {
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return VkExtent2D{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}

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

// Per-frame-in-flight bundle for the live deferred chain. Each frame slot owns
// its own pass instances (which hold per-frame descriptor buffers) and transient
// image pool, so building frame N+1's graph never stomps resources still in use
// by frame N. The main loop waits on the slot's fence before reusing it, which
// makes resetting the pool and rebuilding the descriptor buffers safe.
struct DeferredFrame {
    engine::renderer::MeshPass       mesh;
    engine::renderer::LightingPass   lighting;
    engine::renderer::TonemapPass    tonemap;
    engine::rhi::TransientImagePool  pool;
};

// A unit cube centred at the origin with per-face normals (24 vertices, 36
// indices). Stands in as the demo mesh until glTF scene-graph loading (4.4).
engine::scene::MeshData make_cube_data() {
    using engine::asset::Vertex;
    engine::scene::MeshData d;
    const glm::vec3 face_normals[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (const glm::vec3& n : face_normals) {
        const glm::vec3 up = (std::abs(n.y) > 0.9F) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 t = glm::normalize(glm::cross(up, n));
        const glm::vec3 b = glm::cross(n, t);
        const glm::vec3 c = n * 0.5F;
        const glm::vec3 corners[4] = {
            c - t * 0.5F - b * 0.5F, c + t * 0.5F - b * 0.5F,
            c + t * 0.5F + b * 0.5F, c - t * 0.5F + b * 0.5F};
        const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const auto base = static_cast<std::uint32_t>(d.vertices.size());
        for (int i = 0; i < 4; ++i) {
            Vertex v;
            v.position = corners[i];
            v.normal = n;
            v.uv = uvs[i];
            v.tangent = glm::vec4(t, 1.0F);
            d.vertices.push_back(v);
        }
        for (std::uint32_t idx : {base, base + 1, base + 2, base, base + 2, base + 3}) {
            d.indices.push_back(idx);
        }
    }
    d.bounds = {{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
    d.submeshes = {engine::asset::SubMesh{0, static_cast<std::uint32_t>(d.indices.size()), 0}};
    return d;
}

// World-space AABB over every loaded renderable (mesh local bounds transformed by
// its world matrix). Returns false if the scene has no loaded geometry. Tick the
// scene first so world transforms are current.
bool scene_world_bounds(engine::scene::Scene& scene, glm::vec3& out_min, glm::vec3& out_max) {
    out_min = glm::vec3(std::numeric_limits<float>::max());
    out_max = glm::vec3(std::numeric_limits<float>::lowest());
    bool any = false;
    for (auto [e, t, mr] :
         scene.registry().view<engine::scene::Transform, engine::scene::MeshRenderer>().each()) {
        if (!mr.mesh.valid() || !mr.mesh.is_loaded()) continue;
        const engine::asset::Aabb& ab = mr.mesh->bounds;
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 corner((i & 1) ? ab.max.x : ab.min.x, (i & 2) ? ab.max.y : ab.min.y,
                                   (i & 4) ? ab.max.z : ab.min.z);
            const glm::vec3 w = glm::vec3(t.world * glm::vec4(corner, 1.0F));
            out_min = glm::min(out_min, w);
            out_max = glm::max(out_max, w);
            any = true;
        }
    }
    return any;
}

// Camera world transform that frames the given AABB: looks at its centre from a
// distance sized so the bounding sphere fits the vertical field of view.
glm::mat4 frame_camera_world(const glm::vec3& bmin, const glm::vec3& bmax, float fov_y) {
    const glm::vec3 center = 0.5F * (bmin + bmax);
    const float radius = glm::max(0.001F, 0.5F * glm::length(bmax - bmin));
    const float dist = radius / std::tan(fov_y * 0.5F) * 1.6F;
    const glm::vec3 eye = center + glm::normalize(glm::vec3(0.6F, 0.45F, 1.0F)) * dist;
    return glm::inverse(glm::lookAt(eye, center, glm::vec3(0.0F, 1.0F, 0.0F)));
}

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
    auto hdr = lighting_pass->add_to_graph(graph, *gbuffer, extent, light);
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
        const auto luma = [&](std::uint32_t x, std::uint32_t y) {
            const std::size_t o = (static_cast<std::size_t>(y) * extent.width + x) * 4;
            return static_cast<int>(px[o]) + px[o + 1] + px[o + 2];
        };
        if (luma(extent.width / 2, extent.height / 2) <= 30) {
            result = std::unexpected(
                core::Error{"scene render self-test: cube centre is black (camera not framing it)"});
        } else if (luma(2, 2) > 30) {
            result = std::unexpected(
                core::Error{"scene render self-test: corner is not background-black"});
        }
    }

    vkFreeCommandBuffers(device.handle(), cmd_pool, 1, &cmd);
    vkDestroyCommandPool(device.handle(), cmd_pool, nullptr);
    return result;
}

} // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        k_window_title, k_window_width, k_window_height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (window == nullptr) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    Uint32 sdl_ext_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (sdl_extensions == nullptr) {
        SDL_Log("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto device_result = engine::rhi::Device::create({
        .instance_extensions = {sdl_extensions, sdl_ext_count},
        .enable_validation = k_enable_validation,
        .application_name = k_window_title,
    });
    if (!device_result) {
        std::fprintf(stderr, "[fatal] device creation failed: %s\n",
                     device_result.error().message.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::Device& device = *device_result;
    const VkDevice vk_device = device.handle();

    auto allocator_result = engine::rhi::GpuAllocator::create(device);
    if (!allocator_result) {
        std::fprintf(stderr, "[fatal] allocator creation failed: %s\n",
                     allocator_result.error().message.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::GpuAllocator& allocator = *allocator_result;

    if (std::getenv("QUAT_SELFTEST") != nullptr) {
        if (auto r = engine::asset::run_asset_manager_self_test(); r) {
            std::fprintf(stderr, "[selftest] asset manager OK\n");
        } else {
            std::fprintf(stderr, "[selftest] asset manager FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::scene::run_ecs_self_test(); r) {
            std::fprintf(stderr, "[selftest] ECS world OK\n");
        } else {
            std::fprintf(stderr, "[selftest] ECS world FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::scene::run_scene_self_test(); r) {
            std::fprintf(stderr, "[selftest] scene + systems OK\n");
        } else {
            std::fprintf(stderr, "[selftest] scene + systems FAILED: %s\n",
                         r.error().message.c_str());
        }

        if (auto r = engine::rhi::run_gpu_round_trip_self_test(device, allocator); r) {
            std::fprintf(stderr, "[selftest] VMA GPU round-trip OK\n");
        } else {
            std::fprintf(stderr, "[selftest] FAILED: %s\n", r.error().message.c_str());
        }

        if (auto r = engine::rhi::run_descriptor_buffer_self_test(device, allocator); r) {
            std::fprintf(stderr, "[selftest] descriptor buffer OK\n");
        } else {
            std::fprintf(stderr, "[selftest] descriptor buffer FAILED: %s\n",
                         r.error().message.c_str());
        }

        {
            engine::rhi::TransientImagePool pool(device, allocator);
            if (auto r = engine::rhi::run_render_graph_self_test(device, allocator, pool); r) {
                std::fprintf(stderr, "[selftest] render graph OK\n");
            } else {
                std::fprintf(stderr, "[selftest] render graph FAILED: %s\n",
                             r.error().message.c_str());
            }
        }

        {
            const std::string mesh_spv = std::string(QUAT_COOKED_SHADER_DIR) + "/mesh.spv";
            if (auto r = run_triangle_self_test(device, allocator, mesh_spv); r) {
                std::fprintf(stderr, "[selftest] triangle render OK\n");
            } else {
                std::fprintf(stderr, "[selftest] triangle render FAILED: %s\n",
                             r.error().message.c_str());
            }
        }

        {
            // Transient transfer context for the glTF upload path.
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.queueFamilyIndex = device.queue_families().transfer;
            if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) == VK_SUCCESS) {
                const engine::rhi::TransferContext transfer{device.handle(), pool,
                                                            device.transfer_queue()};
                if (auto r = engine::scene::run_gltf_loader_self_test(allocator, transfer); r) {
                    std::fprintf(stderr, "[selftest] glTF loader OK\n");
                } else {
                    std::fprintf(stderr, "[selftest] glTF loader FAILED: %s\n",
                                 r.error().message.c_str());
                }
                if (auto r = engine::scene::run_texture_loader_self_test(allocator, transfer); r) {
                    std::fprintf(stderr, "[selftest] texture loader OK\n");
                } else {
                    std::fprintf(stderr, "[selftest] texture loader FAILED: %s\n",
                                 r.error().message.c_str());
                }
                if (auto r = engine::asset::run_material_asset_self_test(allocator, transfer); r) {
                    std::fprintf(stderr, "[selftest] material asset OK\n");
                } else {
                    std::fprintf(stderr, "[selftest] material asset FAILED: %s\n",
                                 r.error().message.c_str());
                }
                if (auto r = engine::scene::run_material_extract_self_test(); r) {
                    std::fprintf(stderr, "[selftest] material extract OK\n");
                } else {
                    std::fprintf(stderr, "[selftest] material extract FAILED: %s\n",
                                 r.error().message.c_str());
                }
                if (auto r = engine::scene::run_gltf_scene_self_test(allocator, transfer); r) {
                    std::fprintf(stderr, "[selftest] glTF scene graph OK\n");
                } else {
                    std::fprintf(stderr, "[selftest] glTF scene graph FAILED: %s\n",
                                 r.error().message.c_str());
                }
                if (auto mesh_cache = engine::rhi::PipelineCache::create(device); mesh_cache) {
                    const std::string shader_dir = QUAT_COOKED_SHADER_DIR;
                    if (auto r = engine::renderer::run_mesh_pass_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] GBuffer pass OK\n");
                    } else {
                        std::fprintf(stderr, "[selftest] GBuffer pass FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_lighting_pass_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] lighting pass OK\n");
                    } else {
                        std::fprintf(stderr, "[selftest] lighting pass FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = engine::renderer::run_tonemap_pass_self_test(
                            device, allocator, *mesh_cache, transfer, shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] tonemap pass OK (full deferred chain)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] tonemap pass FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                    if (auto r = run_scene_render_self_test(device, allocator, *mesh_cache, transfer,
                                                            shader_dir);
                        r) {
                        std::fprintf(stderr, "[selftest] scene render OK (camera frames the cube)\n");
                    } else {
                        std::fprintf(stderr, "[selftest] scene render FAILED: %s\n",
                                     r.error().message.c_str());
                    }
                }
                // Optional real-asset smoke test: QUAT_GLTF=<path> loads a glTF/GLB
                // mesh + its textures and reports counts. Exercises the real decode
                // paths (GLB bufferView images, external PNG/JPEG files).
                if (const char* gltf_path = std::getenv("QUAT_GLTF"); gltf_path != nullptr) {
                    if (auto mesh = engine::scene::GltfLoader::load(gltf_path, allocator, transfer);
                        mesh) {
                        std::fprintf(stderr,
                                     "[gltf] %s: %u vertices, %u indices, %zu submeshes\n",
                                     gltf_path, mesh->vertex_count, mesh->index_count,
                                     mesh->submeshes.size());
                    } else {
                        std::fprintf(stderr, "[gltf] mesh load FAILED: %s\n",
                                     mesh.error().message.c_str());
                    }
                    if (auto texs =
                            engine::scene::GltfLoader::load_textures(gltf_path, allocator, transfer);
                        texs) {
                        std::fprintf(stderr, "[gltf] %zu texture(s) loaded\n", texs->size());
                        for (std::size_t i = 0; i < texs->size(); ++i) {
                            std::fprintf(stderr, "[gltf]   tex %zu: %ux%u format=%d valid=%d\n", i,
                                         (*texs)[i].width, (*texs)[i].height,
                                         static_cast<int>((*texs)[i].format),
                                         static_cast<int>((*texs)[i].valid()));
                        }
                    } else {
                        std::fprintf(stderr, "[gltf] texture load FAILED: %s\n",
                                     texs.error().message.c_str());
                    }
                    if (auto mats = engine::scene::GltfLoader::load_material_data(gltf_path);
                        mats) {
                        std::fprintf(stderr, "[gltf] %zu material(s)\n", mats->size());
                        for (std::size_t i = 0; i < mats->size(); ++i) {
                            const auto& p = (*mats)[i].params;
                            std::fprintf(stderr,
                                         "[gltf]   mat %zu: baseColor=(%.2f,%.2f,%.2f,%.2f) "
                                         "metallic=%.2f roughness=%.2f flags=0x%x\n",
                                         i, p.base_color_factor.x, p.base_color_factor.y,
                                         p.base_color_factor.z, p.base_color_factor.w,
                                         p.metallic_factor, p.roughness_factor, p.flags);
                        }
                    } else {
                        std::fprintf(stderr, "[gltf] material load FAILED: %s\n",
                                     mats.error().message.c_str());
                    }
                }
                vkDestroyCommandPool(device.handle(), pool, nullptr);
            } else {
                std::fprintf(stderr, "[selftest] glTF loader FAILED: command pool creation\n");
            }
        }

        if (auto cache = engine::rhi::PipelineCache::create(device); cache) {
            const std::string spv = std::string(QUAT_COOKED_SHADER_DIR) + "/passthrough.spv";
            if (auto shader = cache->load(spv); shader) {
                const auto& refl = (*shader)->reflection;
                std::fprintf(stderr,
                    "[selftest] passthrough loaded: %zu entry points, "
                    "%zu push-constant ranges, %zu descriptor bindings\n",
                    refl.entry_points.size(), refl.push_constant_ranges.size(),
                    refl.descriptor_bindings.size());
                for (const auto& ep : refl.entry_points) {
                    std::fprintf(stderr, "[selftest]   entry: %s (stage 0x%x)\n",
                                 ep.name.c_str(), ep.stage);
                }
            } else {
                std::fprintf(stderr, "[selftest] shader load FAILED: %s\n",
                             shader.error().message.c_str());
            }
        } else {
            std::fprintf(stderr, "[selftest] pipeline cache FAILED: %s\n",
                         cache.error().message.c_str());
        }
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, device.instance(), nullptr, &surface)) {
        std::fprintf(stderr, "[fatal] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto swapchain_result =
        engine::rhi::Swapchain::create(device, surface, window_pixel_extent(window));
    if (!swapchain_result) {
        std::fprintf(stderr, "[fatal] swapchain creation failed: %s\n",
                     swapchain_result.error().message.c_str());
        vkDestroySurfaceKHR(device.instance(), surface, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::Swapchain& swapchain = *swapchain_result;

    // ---- Deferred renderer + demo scene ------------------------------------
    auto fatal = [&](const char* what, const std::string& detail) {
        std::fprintf(stderr, "[fatal] %s: %s\n", what, detail.c_str());
        vkDestroySurfaceKHR(device.instance(), surface, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
    };

    auto pipeline_cache = engine::rhi::PipelineCache::create(device);
    if (!pipeline_cache) {
        fatal("pipeline cache creation failed", pipeline_cache.error().message);
        return 1;
    }

    const std::string shader_dir = QUAT_COOKED_SHADER_DIR;
    const VkFormat swap_format = swapchain.format();

    // One-time transfer context for setup uploads (demo mesh/material + each
    // pass's fallback resources). Destroyed once setup completes.
    VkCommandPool setup_pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.queueFamilyIndex = device.queue_families().transfer;
        vkCreateCommandPool(vk_device, &info, nullptr, &setup_pool);
    }
    const engine::rhi::TransferContext setup_transfer{vk_device, setup_pool,
                                                      device.transfer_queue()};

    // Per-frame-in-flight deferred chains (GBuffer -> lighting -> tonemap). Each
    // slot owns its passes' descriptor buffers and a transient image pool, so
    // building one frame's graph never disturbs the other in-flight frame.
    std::array<DeferredFrame, k_frames_in_flight> frames;
    bool frames_ok = true;
    for (auto& f : frames) {
        auto mesh = engine::renderer::MeshPass::create(device, allocator, *pipeline_cache,
                                                       setup_transfer, shader_dir);
        auto light = engine::renderer::LightingPass::create(device, allocator, *pipeline_cache,
                                                            shader_dir);
        auto tone = engine::renderer::TonemapPass::create(device, allocator, *pipeline_cache,
                                                          shader_dir, swap_format);
        if (!mesh || !light || !tone) {
            frames_ok = false;
            std::fprintf(stderr, "[fatal] deferred pass creation failed: %s\n",
                         (!mesh ? mesh.error() : (!light ? light.error() : tone.error()))
                             .message.c_str());
            break;
        }
        f.mesh = std::move(*mesh);
        f.lighting = std::move(*light);
        f.tonemap = std::move(*tone);
        f.pool = engine::rhi::TransientImagePool(device, allocator);
    }

    // Scene assets live in an AssetManager so MeshRenderer handles keep them
    // alive. QUAT_GLTF=<path> instantiates a real glTF scene graph; otherwise a
    // spinning demo cube stands in.
    engine::asset::AssetManager assets;
    engine::scene::Scene scene;
    entt::entity spin_entity = entt::null; // the demo cube spins; a glTF stays put
    bool scene_ok = true;

    if (const char* gltf_path = std::getenv("QUAT_GLTF"); gltf_path != nullptr) {
        auto roots = engine::scene::GltfLoader::instantiate(gltf_path, allocator, setup_transfer,
                                                            assets, scene);
        if (!roots) {
            std::fprintf(stderr, "[fatal] glTF instantiate failed: %s\n",
                         roots.error().message.c_str());
            scene_ok = false;
        } else {
            std::fprintf(stderr, "[info] loaded '%s': %zu root node(s), %zu entities\n", gltf_path,
                         roots->size(),
                         static_cast<std::size_t>(
                             scene.registry().view<engine::scene::Transform>().size()));
        }
    } else {
        auto cube_handle = assets.load<engine::asset::MeshAsset>(
            "demo/cube", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_cube_data(), allocator, setup_transfer);
            });
        auto mat_handle = assets.load<engine::asset::MaterialAsset>(
            "demo/material", [&](const std::filesystem::path&) {
                engine::asset::PbrMaterialParams params;
                params.base_color_factor = {0.85F, 0.35F, 0.2F, 1.0F};
                params.metallic_factor = 0.0F;
                params.roughness_factor = 0.6F;
                return engine::asset::upload_material(params, engine::asset::MaterialTextures{},
                                                      allocator, setup_transfer);
            });
        if (!cube_handle.is_loaded() || !mat_handle.is_loaded()) {
            scene_ok = false;
        } else {
            spin_entity = scene.create_entity("cube");
            scene.registry().emplace<engine::scene::MeshRenderer>(spin_entity, cube_handle,
                                                                  mat_handle);
        }
    }

    vkDestroyCommandPool(vk_device, setup_pool, nullptr);

    if (!frames_ok || !scene_ok) {
        fatal("deferred renderer setup failed", "see [fatal] lines above");
        return 1;
    }

    // A sun light, and a camera that auto-frames whatever was loaded.
    const entt::entity sun_entity = scene.create_entity("sun");
    scene.registry().emplace<engine::scene::DirectionalLight>(
        sun_entity, glm::normalize(glm::vec3(-0.4F, -1.0F, -0.3F)),
        glm::vec3(1.0F, 0.98F, 0.95F), 3.0F);

    const entt::entity camera_entity = scene.create_entity("camera");
    const engine::scene::Camera& camera =
        scene.registry().emplace<engine::scene::Camera>(camera_entity);
    scene.tick(); // resolve world transforms before measuring the scene bounds
    glm::vec3 bmin{};
    glm::vec3 bmax{};
    const bool has_geo = scene_world_bounds(scene, bmin, bmax);
    const glm::mat4 framed =
        has_geo ? frame_camera_world(bmin, bmax, camera.fov_y)
                : glm::inverse(glm::lookAt(glm::vec3(3.0F, 2.5F, 4.0F), glm::vec3(0.0F),
                                           glm::vec3(0.0F, 1.0F, 0.0F)));

    // Free-fly camera state, seeded from the framed transform so the opening view
    // matches the auto-frame. Updated each frame from input (WASD + right-drag).
    glm::vec3 cam_pos = glm::vec3(framed[3]);
    const glm::vec3 cam_fwd0 = -glm::normalize(glm::vec3(framed[2])); // camera looks down -Z
    float cam_yaw = std::atan2(cam_fwd0.z, cam_fwd0.x);
    float cam_pitch = std::asin(glm::clamp(cam_fwd0.y, -0.999F, 0.999F));
    const float move_speed = has_geo ? glm::max(1.0F, 0.5F * glm::length(bmax - bmin)) : 3.0F;

    // ---- Per-frame command + sync resources --------------------------------
    VkCommandPool command_pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = device.queue_families().graphics;
        vkCreateCommandPool(vk_device, &pool_info, nullptr, &command_pool);
    }

    std::array<VkCommandBuffer, k_frames_in_flight> command_buffers{};
    {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = command_pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = k_frames_in_flight;
        vkAllocateCommandBuffers(vk_device, &alloc, command_buffers.data());
    }

    auto make_semaphore = [vk_device] {
        VkSemaphore s = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(vk_device, &info, nullptr, &s);
        return s;
    };

    // image_available + in-flight fence are per frame-in-flight; render_finished
    // is per swapchain image (it is signalled by the submit that targets that
    // image and waited on by its present).
    std::array<VkSemaphore, k_frames_in_flight> image_available{};
    std::array<VkFence, k_frames_in_flight> in_flight{};
    for (std::uint32_t i = 0; i < k_frames_in_flight; ++i) {
        image_available[i] = make_semaphore();
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(vk_device, &fence_info, nullptr, &in_flight[i]);
    }

    std::vector<VkSemaphore> render_finished(swapchain.image_count(), VK_NULL_HANDLE);
    for (VkSemaphore& s : render_finished) {
        s = make_semaphore();
    }

    auto recreate_render_finished = [&] {
        for (VkSemaphore s : render_finished) {
            vkDestroySemaphore(vk_device, s, nullptr);
        }
        render_finished.assign(swapchain.image_count(), VK_NULL_HANDLE);
        for (VkSemaphore& s : render_finished) {
            s = make_semaphore();
        }
    };

    // ---- Main loop ---------------------------------------------------------
    // QUAT_MAX_FRAMES, when set, auto-quits after that many presented frames.
    // Used for headless smoke tests so clean teardown can be verified.
    std::uint64_t max_frames = 0;
    if (const char* env = std::getenv("QUAT_MAX_FRAMES"); env != nullptr) {
        max_frames = std::strtoull(env, nullptr, 10);
    }

    std::uint64_t presented = 0;
    std::uint32_t frame = 0;
    bool running = true;
    bool needs_recreate = false;
    bool looking = false; // right mouse button held -> mouse-look active
    std::uint64_t last_ticks = SDL_GetTicksNS();

    while (running) {
        // Per-frame accumulated mouse-look delta.
        float look_dx = 0.0F;
        float look_dy = 0.0F;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) running = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                needs_recreate = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    looking = true;
                    SDL_SetWindowRelativeMouseMode(window, true);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_RIGHT) {
                    looking = false;
                    SDL_SetWindowRelativeMouseMode(window, false);
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (looking) {
                    look_dx += event.motion.xrel;
                    look_dy += event.motion.yrel;
                }
                break;
            default:
                break;
            }
        }
        if (!running) break;

        const VkExtent2D pixel_extent = window_pixel_extent(window);
        if (pixel_extent.width == 0 || pixel_extent.height == 0) {
            SDL_Delay(10); // minimized — nothing to draw
            continue;
        }

        if (needs_recreate) {
            if (auto r = swapchain.recreate(pixel_extent); !r) {
                std::fprintf(stderr, "[fatal] swapchain recreate failed: %s\n",
                             r.error().message.c_str());
                break;
            }
            recreate_render_finished();
            needs_recreate = false;
        }

        vkWaitForFences(vk_device, 1, &in_flight[frame], VK_TRUE, UINT64_MAX);

        std::uint32_t image_index = 0;
        VkResult acquire = vkAcquireNextImageKHR(
            vk_device, swapchain.handle(), UINT64_MAX, image_available[frame],
            VK_NULL_HANDLE, &image_index);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            needs_recreate = true;
            continue;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            std::fprintf(stderr, "[fatal] vkAcquireNextImageKHR failed (%d)\n",
                         static_cast<int>(acquire));
            break;
        }

        vkResetFences(vk_device, 1, &in_flight[frame]);

        VkCommandBuffer cmd = command_buffers[frame];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        // Build this frame's deferred graph into the slot's own resources and
        // render the scene to the acquired swapchain image. The slot's fence was
        // waited above, so resetting its transient pool and rebuilding the passes'
        // descriptor buffers does not race the other in-flight frame.
        DeferredFrame& fr = frames[frame];
        fr.pool.reset();

        const VkExtent2D draw_extent = swapchain.extent();
        const float aspect =
            draw_extent.height == 0
                ? 1.0F
                : static_cast<float>(draw_extent.width) / static_cast<float>(draw_extent.height);

        // Free-fly camera: right-drag looks, WASD moves, Q/E down/up, Shift = fast.
        const std::uint64_t now_ticks = SDL_GetTicksNS();
        const float dt = static_cast<float>(now_ticks - last_ticks) * 1e-9F;
        last_ticks = now_ticks;

        constexpr float look_sensitivity = 0.0025F;
        cam_yaw += look_dx * look_sensitivity;
        cam_pitch = glm::clamp(cam_pitch - look_dy * look_sensitivity, glm::radians(-89.0F),
                               glm::radians(89.0F));
        const glm::vec3 cam_fwd(std::cos(cam_pitch) * std::cos(cam_yaw), std::sin(cam_pitch),
                                std::cos(cam_pitch) * std::sin(cam_yaw));
        const glm::vec3 world_up(0.0F, 1.0F, 0.0F);
        const glm::vec3 cam_right = glm::normalize(glm::cross(cam_fwd, world_up));

        const bool* keys = SDL_GetKeyboardState(nullptr);
        const auto axis = [&](SDL_Scancode pos, SDL_Scancode neg) {
            return (keys[pos] ? 1.0F : 0.0F) - (keys[neg] ? 1.0F : 0.0F);
        };
        const glm::vec3 move = cam_fwd * axis(SDL_SCANCODE_W, SDL_SCANCODE_S) +
                               cam_right * axis(SDL_SCANCODE_D, SDL_SCANCODE_A) +
                               world_up * axis(SDL_SCANCODE_E, SDL_SCANCODE_Q);
        const float speed = move_speed * (keys[SDL_SCANCODE_LSHIFT] ? 4.0F : 1.0F);
        if (move != glm::vec3(0.0F)) {
            cam_pos += glm::normalize(move) * speed * dt;
        }
        scene.registry().get<engine::scene::Transform>(camera_entity).local =
            glm::inverse(glm::lookAt(cam_pos, cam_pos + cam_fwd, world_up));

        // Spin the demo cube (if present), tick the scene, then derive camera +
        // light from the ECS.
        if (spin_entity != entt::null) {
            scene.registry().get<engine::scene::Transform>(spin_entity).local =
                glm::rotate(glm::mat4(1.0F), static_cast<float>(presented) * 0.02F,
                            glm::normalize(glm::vec3(0.3F, 1.0F, 0.2F)));
        }
        scene.tick();
        const engine::scene::CameraMatrices cam =
            engine::scene::camera_system(scene.registry(), aspect);

        engine::renderer::DirectionalLightParams light;
        const auto light_view = scene.registry().view<const engine::scene::DirectionalLight>();
        for (const entt::entity e : light_view) {
            const auto& dl = light_view.get<const engine::scene::DirectionalLight>(e);
            light.direction = glm::vec4(-glm::normalize(dl.direction), 0.0F);
            light.color = glm::vec4(dl.color, dl.intensity);
            break;
        }

        engine::rhi::RenderGraph graph(fr.pool);
        auto gbuffer = fr.mesh.add_to_graph(graph, draw_extent, cam.view_proj, scene.draw_list());
        if (!gbuffer) {
            std::fprintf(stderr, "[fatal] gbuffer pass: %s\n", gbuffer.error().message.c_str());
            break;
        }
        auto hdr = fr.lighting.add_to_graph(graph, *gbuffer, draw_extent, light);
        if (!hdr) {
            std::fprintf(stderr, "[fatal] lighting pass: %s\n", hdr.error().message.c_str());
            break;
        }
        const engine::rhi::ResourceHandle backbuffer = graph.import_image(
            "swapchain", swapchain.images()[image_index], swapchain.image_views()[image_index],
            swapchain.format(), draw_extent, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        if (auto r = fr.tonemap.add_to_graph(graph, *hdr, backbuffer, draw_extent); !r) {
            std::fprintf(stderr, "[fatal] tonemap pass: %s\n", r.error().message.c_str());
            break;
        }
        if (auto compiled = graph.compile(); !compiled) {
            std::fprintf(stderr, "[fatal] render graph compile failed: %s\n",
                         compiled.error().message.c_str());
            break;
        }
        graph.execute(cmd);

        vkEndCommandBuffer(cmd);

        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = image_available[frame];
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = render_finished[image_index];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_info.commandBuffer = cmd;

        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmd_info;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        vkQueueSubmit2(device.graphics_queue(), 1, &submit, in_flight[frame]);

        VkSwapchainKHR sc = swapchain.handle();
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished[image_index];
        present.swapchainCount = 1;
        present.pSwapchains = &sc;
        present.pImageIndices = &image_index;
        VkResult present_result = vkQueuePresentKHR(device.graphics_queue(), &present);
        if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
            needs_recreate = true;
        } else if (present_result != VK_SUCCESS) {
            std::fprintf(stderr, "[fatal] vkQueuePresentKHR failed (%d)\n",
                         static_cast<int>(present_result));
            break;
        }

        frame = (frame + 1) % k_frames_in_flight;
        if (max_frames != 0 && ++presented >= max_frames) {
            running = false;
        }
    }

    // ---- Teardown ----------------------------------------------------------
    vkDeviceWaitIdle(vk_device);
    for (VkSemaphore s : render_finished) {
        vkDestroySemaphore(vk_device, s, nullptr);
    }
    for (std::uint32_t i = 0; i < k_frames_in_flight; ++i) {
        vkDestroySemaphore(vk_device, image_available[i], nullptr);
        vkDestroyFence(vk_device, in_flight[i], nullptr);
    }
    vkDestroyCommandPool(vk_device, command_pool, nullptr);
    swapchain = engine::rhi::Swapchain{};     // destroy swapchain before surface
    vkDestroySurfaceKHR(device.instance(), surface, nullptr);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
