// Swapchain extent selection on high-density displays (fix/hidpi-rendering).
//
// choose_swapchain_extent() is pure over the surface capabilities, so the
// rules are tested with injected capabilities: a surface that reports its
// size (macOS's CAMetalLayer drawable, Win32, X11) and one that leaves it to
// the swapchain (Wayland). The Gpu* test builds a real swapchain on a
// VK_EXT_headless_surface (whose extent is the swapchain's choice, as on
// Wayland) for a window of the same point size at pixel density 1 and 2.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <vector>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/core/pixel_scale.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/swapchain.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

using engine::rhi::choose_swapchain_extent;

[[nodiscard]] VkSurfaceCapabilitiesKHR caps(VkExtent2D current) {
    VkSurfaceCapabilitiesKHR c{};
    c.currentExtent = current;
    c.minImageExtent = {1, 1};
    c.maxImageExtent = {16384, 16384};
    return c;
}

[[nodiscard]] bool same(VkExtent2D a, VkExtent2D b) {
    return a.width == b.width && a.height == b.height;
}

// The window's framebuffer size for a point size at a pixel density, as
// SDL_GetWindowSizeInPixels reports it.
[[nodiscard]] VkExtent2D window_pixels(float w_points, float h_points, float density) {
    const engine::core::PixelExtent px =
        engine::core::points_to_pixel_extent(w_points, h_points, density, density, 1, 16384);
    return {px.width, px.height};
}

TEST(SwapchainExtent, SurfaceDefinedExtentIsUsedAsIs) {
    // macOS: the layer's drawable size, already in pixels at the density.
    const VkExtent2D e = choose_swapchain_extent(caps({2560, 1600}), {1280, 800});
    EXPECT_TRUE(same(e, {2560, 1600}));
}

TEST(SwapchainExtent, UndefinedExtentFollowsTheWindowPixels) {
    const VkSurfaceCapabilitiesKHR c = caps({UINT32_MAX, UINT32_MAX});
    EXPECT_TRUE(same(choose_swapchain_extent(c, window_pixels(1280, 800, 2.0F)), {2560, 1600}));
    EXPECT_TRUE(same(choose_swapchain_extent(c, window_pixels(1280, 800, 1.0F)), {1280, 800}));
    // A 150 % output with an odd point size rounds per axis.
    EXPECT_TRUE(same(choose_swapchain_extent(c, window_pixels(1001, 667, 1.5F)), {1502, 1001}));
}

TEST(SwapchainExtent, UndefinedExtentIsClampedToTheSurfaceLimits) {
    VkSurfaceCapabilitiesKHR c = caps({UINT32_MAX, UINT32_MAX});
    c.maxImageExtent = {4096, 4096};
    c.minImageExtent = {8, 8};
    EXPECT_TRUE(same(choose_swapchain_extent(c, {5120, 2}), {4096, 8}));
}

[[nodiscard]] bool instance_extension_available(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    return std::any_of(exts.begin(), exts.end(), [name](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

// The same window at pixel density 2 gets a swapchain twice its point size
// per axis; moving it back to a density-1 display (recreate) halves it, and
// requested_extent() tracks the window pixel size the frame loop compares
// against to notice a stale swapchain.
TEST(GpuSwapchainExtent, PixelDensityTwoGetsATwiceSizedSwapchain) {
    QUAT_REQUIRE_GPU(g);
    if (!instance_extension_available(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) {
        GTEST_SKIP() << "VK_EXT_headless_surface is not available";
    }
    const std::array<const char*, 2> instance_exts{VK_KHR_SURFACE_EXTENSION_NAME,
                                                   VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    auto device = engine::rhi::Device::create({
        .instance_extensions = instance_exts,
        .enable_validation = true,
        .application_name = "engine_tests_hidpi",
        .create_surface = [](VkInstance instance)
            -> std::expected<VkSurfaceKHR, engine::core::Error> {
            const auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
            if (create == nullptr) {
                return std::unexpected(engine::core::Error{"no vkCreateHeadlessSurfaceEXT"});
            }
            VkHeadlessSurfaceCreateInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (create(instance, &info, nullptr, &surface) != VK_SUCCESS) {
                return std::unexpected(engine::core::Error{"vkCreateHeadlessSurfaceEXT failed"});
            }
            return surface;
        },
    });
    ASSERT_OK(device);
    VkSurfaceCapabilitiesKHR surface_caps{};
    ASSERT_EQ(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical_device(),
                                                        device->surface(), &surface_caps),
              VK_SUCCESS);
    if (surface_caps.currentExtent.width != UINT32_MAX) {
        GTEST_SKIP() << "this driver's headless surface dictates its own extent";
    }

    const VkExtent2D retina = window_pixels(160.0F, 90.0F, 2.0F);
    auto swapchain = engine::rhi::Swapchain::create(*device, device->surface(), retina);
    ASSERT_OK(swapchain);
    EXPECT_TRUE(same(swapchain->extent(), {320, 180}));
    EXPECT_TRUE(same(swapchain->requested_extent(), retina));

    const VkExtent2D standard = window_pixels(160.0F, 90.0F, 1.0F);
    ASSERT_OK(swapchain->recreate(standard));
    EXPECT_TRUE(same(swapchain->extent(), {160, 90}));
    EXPECT_TRUE(same(swapchain->requested_extent(), standard));

    // Moved-from swapchains carry the requested extent along.
    engine::rhi::Swapchain moved = std::move(*swapchain);
    EXPECT_TRUE(same(moved.requested_extent(), standard));
    vkDeviceWaitIdle(device->handle());
}

} // namespace
