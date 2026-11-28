#include "engine/rhi/swapchain.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <utility>
#include <vector>

#include "engine/rhi/device.hpp"
#include "engine/rhi/formats.hpp"
#include "engine/rhi/queue_submit.hpp"

namespace engine::rhi {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] VkSurfaceFormatKHR choose_format(VkPhysicalDevice gpu, VkSurfaceKHR surface) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats.data());

    for (const VkSurfaceFormatKHR& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB,
                                                VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                           : formats.front();
}

[[nodiscard]] VkExtent2D clamp_extent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D wanted) {
    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;
    }
    return VkExtent2D{
        std::clamp(wanted.width, caps.minImageExtent.width, caps.maxImageExtent.width),
        std::clamp(wanted.height, caps.minImageExtent.height, caps.maxImageExtent.height)};
}

// Whether swapchain images of `format` + `usage` can be created mutable-format
// with the view formats in `list` (the implied image parameters of
// VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR, VUID-VkSwapchainCreateInfoKHR-
// imageFormat-01778).
[[nodiscard]] bool supports_mutable_format(VkPhysicalDevice gpu, VkFormat format,
                                           VkImageUsageFlags usage,
                                           const VkImageFormatListCreateInfo& list) {
    VkPhysicalDeviceImageFormatInfo2 query{};
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    query.pNext = &list;
    query.format = format;
    query.type = VK_IMAGE_TYPE_2D;
    query.tiling = VK_IMAGE_TILING_OPTIMAL;
    query.usage = usage;
    query.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    VkImageFormatProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    return vkGetPhysicalDeviceImageFormatProperties2(gpu, &query, &props) == VK_SUCCESS;
}

[[nodiscard]] VkImageView make_view(VkDevice device, VkImage image, VkFormat format) {
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return view;
}

} // namespace

std::expected<Swapchain, core::Error>
Swapchain::create(const Device& device, VkSurfaceKHR surface, VkExtent2D window_extent) {
    Swapchain sc;
    sc.device_  = &device;
    sc.surface_ = surface;
    if (auto r = sc.build(window_extent); !r) {
        return std::unexpected(r.error());
    }
    return sc;
}

std::expected<void, core::Error> Swapchain::build(VkExtent2D window_extent) {
    VkPhysicalDevice gpu = device_->physical_device();
    const QueueFamilies& families = device_->queue_families();

    // The device must have been selected for this surface
    // (VUID-VkSwapchainCreateInfoKHR-surface-01270): presenting from a queue
    // that cannot present to it is undefined.
    if (surface_ == VK_NULL_HANDLE) return fail("swapchain: no surface");
    if (families.present == VK_QUEUE_FAMILY_IGNORED) {
        return fail("swapchain: the device was created without a surface "
                    "(pass Device::CreateInfo::create_surface)");
    }
    VkBool32 supported = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(gpu, families.present, surface_, &supported) !=
            VK_SUCCESS ||
        supported != VK_TRUE) {
        return fail(std::format("swapchain: queue family {} of the selected GPU cannot present "
                                "to this surface",
                                families.present));
    }

    VkSurfaceCapabilitiesKHR caps{};
    if (VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface_, &caps);
        r != VK_SUCCESS) {
        return fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
    }

    const VkSurfaceFormatKHR surface_format = choose_format(gpu, surface_);
    const VkExtent2D extent = clamp_extent(caps, window_extent);

    std::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = surface_format.format;
    info.imageColorSpace = surface_format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    // COLOR_ATTACHMENT for render passes, TRANSFER_DST for the TAA resolve copy,
    // TRANSFER_SRC for headless screenshot capture (QUAT_SCREENSHOT).
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // An sRGB swapchain is also viewable as its UNORM twin when the device
    // has VK_KHR_swapchain_mutable_format: the editor UI blends in gamma
    // space through that view (ImGuiPass), everything else is unchanged.
    const VkFormat unorm = unorm_of_srgb(surface_format.format);
    const std::array<VkFormat, 2> view_formats{surface_format.format, unorm};
    VkImageFormatListCreateInfo format_list{};
    format_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
    format_list.viewFormatCount = static_cast<std::uint32_t>(view_formats.size());
    format_list.pViewFormats = view_formats.data();
    const bool mutable_format =
        unorm != VK_FORMAT_UNDEFINED && device_->swapchain_mutable_format() &&
        supports_mutable_format(gpu, surface_format.format, info.imageUsage, format_list);
    if (mutable_format) {
        info.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
        info.pNext = &format_list;
    }
    // Rendered on the graphics queue, presented from the present queue. When
    // those are different families (rare: no family both draws and presents)
    // the images are shared concurrently, so no ownership transfer is needed.
    const std::array<std::uint32_t, 2> sharing_families{families.graphics, families.present};
    if (families.graphics != families.present) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = static_cast<std::uint32_t>(sharing_families.size());
        info.pQueueFamilyIndices = sharing_families.data();
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO (vsync) is the guaranteed default. QUAT_NO_VSYNC=1 prefers MAILBOX,
    // then IMMEDIATE, for uncapped frame-rate measurement.
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (std::getenv("QUAT_NO_VSYNC") != nullptr) {
        std::uint32_t mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface_, &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface_, &mode_count, modes.data());
        for (const VkPresentModeKHR want :
             {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::find(modes.begin(), modes.end(), want) != modes.end()) {
                info.presentMode = want;
                break;
            }
        }
    }
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;

    if (VkResult r = vkCreateSwapchainKHR(device_->handle(), &info, nullptr, &swapchain_);
        r != VK_SUCCESS) {
        return fail(std::format("vkCreateSwapchainKHR failed ({})", static_cast<int>(r)));
    }

    format_ = surface_format.format;
    extent_ = extent;

    std::uint32_t actual_count = 0;
    vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &actual_count, nullptr);
    images_.resize(actual_count);
    vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &actual_count, images_.data());

    image_views_.resize(actual_count, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < actual_count; ++i) {
        image_views_[i] = make_view(device_->handle(), images_[i], format_);
        if (image_views_[i] == VK_NULL_HANDLE) {
            return fail("vkCreateImageView failed for swapchain image");
        }
    }
    if (mutable_format) {
        unorm_image_views_.resize(actual_count, VK_NULL_HANDLE);
        for (std::uint32_t i = 0; i < actual_count; ++i) {
            unorm_image_views_[i] = make_view(device_->handle(), images_[i], unorm);
            if (unorm_image_views_[i] == VK_NULL_HANDLE) {
                return fail("vkCreateImageView failed for a swapchain image's UNORM view");
            }
        }
    }
    return {};
}

std::expected<void, core::Error> Swapchain::recreate(VkExtent2D window_extent) {
    // A lost device is reported, not rebuilt against (issue #110).
    if (const VkResult r = vkDeviceWaitIdle(device_->handle()); r != VK_SUCCESS) {
        return fail(queue_error_message("swapchain recreate", "vkDeviceWaitIdle", r));
    }
    destroy_resources();
    return build(window_extent);
}

void Swapchain::destroy_resources() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (VkImageView view : image_views_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_->handle(), view, nullptr);
        }
    }
    image_views_.clear();
    for (VkImageView view : unorm_image_views_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_->handle(), view, nullptr);
        }
    }
    unorm_image_views_.clear();
    images_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_->handle(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void Swapchain::destroy() noexcept {
    destroy_resources();
    device_ = nullptr;
    surface_ = VK_NULL_HANDLE;
}

Swapchain::~Swapchain() {
    destroy();
}

Swapchain::Swapchain(Swapchain&& other) noexcept {
    *this = std::move(other);
}

Swapchain& Swapchain::operator=(Swapchain&& other) noexcept {
    if (this != &other) {
        destroy();
        device_      = std::exchange(other.device_, nullptr);
        surface_     = std::exchange(other.surface_, VK_NULL_HANDLE);
        swapchain_   = std::exchange(other.swapchain_, VK_NULL_HANDLE);
        format_      = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
        extent_      = other.extent_;
        images_      = std::move(other.images_);
        image_views_ = std::move(other.image_views_);
        unorm_image_views_ = std::move(other.unorm_image_views_);
    }
    return *this;
}

} // namespace engine::rhi
