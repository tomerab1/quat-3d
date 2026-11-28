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
// VK_ICD_FILENAMES, VK_ADD_DRIVER_FILES), in that order. The engine honours
// them, as any Vulkan application does; they are only reported.
[[nodiscard]] std::vector<LoaderDriverOverride> loader_driver_overrides(const GetEnvFn& getenv);

// One log line: device, driver name and version text, and where the loader
// was told to look for drivers ("loader default discovery" when nowhere).
[[nodiscard]] std::string describe_driver(const DriverInfo& driver,
                                          const std::vector<LoaderDriverOverride>& overrides);

} // namespace engine::rhi
