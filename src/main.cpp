// quat-3d engine entry point.
//
// Phase 1, Slice 1.4: bring up the swapchain and a present loop. Each frame the
// acquired swapchain image is cleared to black (via synchronization2 barriers
// and vkQueueSubmit2) and presented. Two frames in flight. No real rendering
// yet — that begins with the render graph and first triangle in Phase 2.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
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

VkImageMemoryBarrier2 image_barrier(VkImage image,
                                    VkImageLayout old_layout, VkImageLayout new_layout,
                                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

void cmd_pipeline_barrier(VkCommandBuffer cmd, const VkImageMemoryBarrier2& barrier) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// Records: undefined -> transfer-dst, clear to black, transfer-dst -> present.
void record_clear_to_black(VkCommandBuffer cmd, VkImage image) {
    cmd_pipeline_barrier(cmd, image_barrier(
        image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE, 0,
        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT));

    const VkClearColorValue black{{0.0F, 0.0F, 0.0F, 1.0F}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

    cmd_pipeline_barrier(cmd, image_barrier(
        image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_NONE, 0));
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
        record_clear_to_black(cmd, swapchain.images()[image_index]);
        vkEndCommandBuffer(cmd);

        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = image_available[frame];
        wait.stageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;

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
