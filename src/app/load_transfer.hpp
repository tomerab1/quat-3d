#pragma once

// One-off upload context for work outside the frame loop (editor scene
// loads, UI font atlas re-uploads): a transient command pool on the graphics
// queue (mip blits need a graphics-capable queue), like the startup path.

#include <cstdio>
#include <utility>

#include <vulkan/vulkan.h>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp" // TransferContext

namespace quat::app {

// Runs `fn(const engine::rhi::TransferContext&)`; returns false (and skips it) when
// the command pool cannot be created.
template <typename Fn>
[[nodiscard]] bool with_load_transfer(const engine::rhi::Device& device, Fn&& fn) {
    const VkDevice          vk_device = device.handle();
    VkCommandPool           pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device.queue_families().graphics;
    if (const VkResult r = vkCreateCommandPool(vk_device, &pool_info, nullptr, &pool);
        r != VK_SUCCESS) {
        std::fprintf(stderr, "[warn] upload skipped: vkCreateCommandPool failed (%d)\n",
                     static_cast<int>(r));
        return false;
    }
    const engine::rhi::TransferContext transfer{vk_device, pool, device.graphics_queue(),
                                                device.max_sampler_anisotropy(), device.texture_caps()};
    std::forward<Fn>(fn)(transfer);
    vkDestroyCommandPool(vk_device, pool, nullptr);
    return true;
}

} // namespace quat::app
