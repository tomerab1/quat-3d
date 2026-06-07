// quat-3d engine entry point.
//
// Phase 1, Slice 1.3: open an SDL3 Vulkan window, create the RHI device
// (instance + debug messenger + physical/logical device + queues), then run an
// event loop that exits on window-close or ESC. No rendering yet — the
// swapchain and present loop arrive in Slice 1.4.

#include <cstdio>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "engine/rhi/device.hpp"

namespace {

constexpr int   k_window_width  = 1280;
constexpr int   k_window_height = 720;
constexpr char  k_window_title[] = "quat-3d";

#ifndef NDEBUG
constexpr bool k_enable_validation = true;
#else
constexpr bool k_enable_validation = false;
#endif

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

    // Instance extensions required by SDL to present to this window's surface.
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
    std::fprintf(stderr, "[main] Vulkan device ready: %s\n", device.properties().deviceName);

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) {
                    running = false;
                }
                break;
            default:
                break;
            }
        }
    }

    // device tears down (logical device, debug messenger, instance) on scope exit.
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
