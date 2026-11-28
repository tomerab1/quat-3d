#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"

namespace engine::rhi {

class Device;

// The swapchain image extent for a surface with capabilities `caps` and a
// window measured at `window_pixels` (SDL_GetWindowSizeInPixels: the
// framebuffer size, which on a high-density display is the window's point
// size times its pixel density). A surface that reports its current extent
// gets exactly that; one that leaves it to the swapchain (0xFFFFFFFF, as on
// Wayland) gets the window's pixel size clamped to the supported range.
[[nodiscard]] VkExtent2D choose_swapchain_extent(const VkSurfaceCapabilitiesKHR& caps,
                                                 VkExtent2D window_pixels);

// Owns the VkSwapchainKHR and its image views. The surface is owned elsewhere
// (normally the Device, via CreateInfo::create_surface); the swapchain only
// references it, and creation fails clearly when the device's present queue
// family cannot present to it. FIFO present mode (always available),
// preferring a B8G8R8A8_SRGB surface format. Images are rendered on the
// graphics queue and presented with Device::present_queue(). With
// VK_KHR_swapchain_mutable_format (Device::swapchain_mutable_format) an sRGB
// swapchain is created mutable-format and each image also gets a view in its
// UNORM twin format (unorm_image_views), through which the editor UI blends
// in gamma space.
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
    // The window pixel extent the swapchain was last built for (the argument
    // of create / recreate); differs from extent() when the surface dictates
    // its own size. A different window pixel size means the swapchain is stale.
    [[nodiscard]] VkExtent2D requested_extent() const { return requested_extent_; }
    [[nodiscard]] std::uint32_t image_count() const {
        return static_cast<std::uint32_t>(images_.size());
    }
    [[nodiscard]] std::span<const VkImage> images() const { return images_; }
    [[nodiscard]] std::span<const VkImageView> image_views() const { return image_views_; }
    // One UNORM-format view per image (same order as images()) of a
    // mutable-format sRGB swapchain; empty otherwise (no extension, or the
    // surface format is not sRGB). Hand it to RenderGraph::set_unorm_view.
    [[nodiscard]] std::span<const VkImageView> unorm_image_views() const {
        return unorm_image_views_;
    }

private:
    [[nodiscard]] std::expected<void, core::Error> build(VkExtent2D window_extent);
    void destroy_resources() noexcept;
    void destroy() noexcept;

    const Device*              device_  = nullptr;   // non-owning
    VkSurfaceKHR               surface_ = VK_NULL_HANDLE; // non-owning
    VkSwapchainKHR             swapchain_ = VK_NULL_HANDLE;
    VkFormat                   format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D                 extent_{};
    VkExtent2D                 requested_extent_{};
    std::vector<VkImage>       images_;
    std::vector<VkImageView>   image_views_;
    std::vector<VkImageView>   unorm_image_views_;
};

} // namespace engine::rhi
