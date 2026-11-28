#pragma once

// Which Vulkan driver the engine is running on, logged once at device
// creation. A bug report then names the driver at a glance: on macOS, a
// LunarG Vulkan SDK environment (VK_DRIVER_FILES / VK_ICD_FILENAMES from its
// setup-env.sh) replaces the MoltenVK bundled in the .app with the SDK's own,
// and translation bugs differ between MoltenVK versions (issue #130).

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

namespace engine::rhi {

// VkPhysicalDeviceDriverProperties (core in Vulkan 1.2) plus the device name.
struct DriverInfo {
    std::string device_name;
    std::string driver_name; // e.g. "MoltenVK", "llvmpipe", "NVIDIA"
    std::string driver_info; // the driver's version text, e.g. MoltenVK's "1.4.1"
    VkDriverId  driver_id = static_cast<VkDriverId>(0);
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0; // vendor-encoded
};

// A loader environment variable that overrides (or adds to) the drivers the
// Vulkan loader would otherwise find, and its value.
struct LoaderDriverOverride {
    std::string_view variable;
    std::string      value;
};

// Reads an environment variable; nullptr when unset (std::getenv's contract).
using GetEnvFn = std::function<const char*(const char* name)>;

[[nodiscard]] DriverInfo query_driver_info(VkPhysicalDevice physical_device);

// The set loader driver-selection variables (VK_DRIVER_FILES,
// VK_ICD_FILENAMES, VK_ADD_DRIVER_FILES, VK_LOADER_DRIVERS_SELECT,
// VK_LOADER_DRIVERS_DISABLE), in that order. The engine honours
// them, as any Vulkan application does; they are only reported.
[[nodiscard]] std::vector<LoaderDriverOverride> loader_driver_overrides(const GetEnvFn& getenv);

// One log line: device, driver name and version text, and where the loader
// was told to look for drivers ("loader default discovery" when nowhere).
[[nodiscard]] std::string describe_driver(const DriverInfo& driver,
                                          const std::vector<LoaderDriverOverride>& overrides);

// Whether a VkImageView that reinterprets a swapchain image's format (the
// UNORM twin of an sRGB image, VK_KHR_swapchain_mutable_format) keeps
// addressing that image every time it is acquired.
//
// MoltenVK before 1.4.3 does not guarantee it: the view caches a Metal
// texture view of the CAMetalDrawable the image had when the view was first
// used, while the image gets whichever drawable the layer hands out on each
// acquire. Rendering through the view then lands in a drawable that is not
// the one being presented. Upstream fixed it in June 2026 ("Fix caching of
// MTLTexture object behind VkImageViews vended for swapchain images",
// KhronosGroup/MoltenVK#2747), which the 1.4.2 release contains, but every
// main-branch build since December 2025 already reports 1.4.2, including
// the MoltenVK in LunarG Vulkan SDK 1.4.341. So 1.4.2 cannot be trusted and
// the first version known to carry the fix in every build is 1.4.3.
[[nodiscard]] bool swapchain_format_views_track_images(const DriverInfo& driver);

} // namespace engine::rhi
