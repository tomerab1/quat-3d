#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"

namespace engine::rhi {

class Device;

// Owns the VkSwapchainKHR and its image views. The surface is created and owned
// by the caller; the swapchain only references it. FIFO present mode (always
// available), preferring a B8G8R8A8_SRGB surface format.
class Swapchain {
public:
    [[nodiscard]] static std::expected<Swapchain, core::Error>
    create(const Device& device, VkSurfaceKHR surface, VkExtent2D window_extent);

    Swapchain() = default;
    ~Swapchain();

    Swapchain(Swapchain&& other) noexcept;
    Swapchain& operator=(Swapchain&& other) noexcept;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Tears down and rebuilds the swapchain at a new size (e.g. after resize or
    // an out-of-date acquire/present). Waits for the device to be idle first.
    [[nodiscard]] std::expected<void, core::Error> recreate(VkExtent2D window_extent);

    [[nodiscard]] VkSwapchainKHR handle() const { return swapchain_; }
    [[nodiscard]] VkFormat format() const { return format_; }
    [[nodiscard]] VkExtent2D extent() const { return extent_; }
    [[nodiscard]] std::uint32_t image_count() const {
        return static_cast<std::uint32_t>(images_.size());
    }
    [[nodiscard]] std::span<const VkImage> images() const { return images_; }
    [[nodiscard]] std::span<const VkImageView> image_views() const { return image_views_; }

private:
    [[nodiscard]] std::expected<void, core::Error> build(VkExtent2D window_extent);
    void destroy_resources() noexcept;
    void destroy() noexcept;

    const Device*              device_  = nullptr;   // non-owning
    VkSurfaceKHR               surface_ = VK_NULL_HANDLE; // non-owning
    VkSwapchainKHR             swapchain_ = VK_NULL_HANDLE;
    VkFormat                   format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D                 extent_{};
    std::vector<VkImage>       images_;
    std::vector<VkImageView>   image_views_;
};

} // namespace engine::rhi
