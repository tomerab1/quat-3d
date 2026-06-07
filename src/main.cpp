// quat-3d engine entry point.
//
// Phase 2, Slice 2.5: a hard-coded triangle is drawn via the render graph. Each
// frame builds a RenderGraph that imports the acquired swapchain image and runs
// a single GraphicsPass (dynamic rendering) that clears to black and draws the
// triangle from mesh.slang. The graph inserts the layout barriers and the
// trailing transition to PRESENT_SRC. Two frames in flight.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "engine/rhi/descriptor_buffer.hpp"
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

    // ---- Triangle pipeline -------------------------------------------------
    auto pipeline_cache = engine::rhi::PipelineCache::create(device);
    if (!pipeline_cache) {
        std::fprintf(stderr, "[fatal] pipeline cache creation failed: %s\n",
                     pipeline_cache.error().message.c_str());
        vkDestroySurfaceKHR(device.instance(), surface, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    const std::string mesh_spv = std::string(QUAT_COOKED_SHADER_DIR) + "/mesh.spv";
    auto mesh_shader = pipeline_cache->load(mesh_spv);
    if (!mesh_shader) {
        std::fprintf(stderr, "[fatal] mesh shader load failed: %s\n",
                     mesh_shader.error().message.c_str());
        vkDestroySurfaceKHR(device.instance(), surface, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    const VkFormat color_format = swapchain.format();
    auto pipeline_result = engine::rhi::GraphicsPipeline::create(
        device, pipeline_cache->handle(),
        {
            .vertex = *mesh_shader,
            .fragment = *mesh_shader,
            .color_formats = {&color_format, 1},
        });
    if (!pipeline_result) {
        std::fprintf(stderr, "[fatal] triangle pipeline creation failed: %s\n",
                     pipeline_result.error().message.c_str());
        vkDestroySurfaceKHR(device.instance(), surface, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::GraphicsPipeline& triangle_pipeline = *pipeline_result;

    // Transient image pool backing per-frame render graphs (no transient images
    // are declared yet — the triangle renders straight into the swapchain image).
    engine::rhi::TransientImagePool transient_pool(device, allocator);

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

    while (running) {
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

        // Build this frame's render graph: import the swapchain image and draw
        // the triangle into it via a single graphics pass. The graph owns the
        // UNDEFINED -> COLOR_ATTACHMENT -> PRESENT_SRC layout transitions.
        engine::rhi::RenderGraph graph(transient_pool);
        const VkExtent2D draw_extent = swapchain.extent();
        const VkImageView color_view = swapchain.image_views()[image_index];
        const VkPipeline pipeline = triangle_pipeline.handle();
        const engine::rhi::ResourceHandle backbuffer = graph.import_image(
            "swapchain", swapchain.images()[image_index], color_view,
            swapchain.format(), draw_extent,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        graph.add_pass("triangle", engine::rhi::PassType::graphics)
            .writes(backbuffer, engine::rhi::ResourceUsage::color_attachment)
            .execute([=](engine::rhi::PassContext& ctx) {
                record_triangle(ctx.cmd(), color_view, draw_extent, pipeline);
            });
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
