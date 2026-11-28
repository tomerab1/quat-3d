// Driver identification — see engine/rhi/driver_info.hpp.

#include "engine/rhi/driver_info.hpp"

#include <array>
#include <cstring>
#include <format>

namespace engine::rhi {

namespace {

// A fixed-size, NUL-terminated Vulkan string (bounded even if a driver
// forgets the terminator).
template <std::size_t N>
[[nodiscard]] std::string bounded(const char (&text)[N]) {
    return {text, strnlen(text, N)};
}

constexpr std::array<std::string_view, 5> k_driver_variables{
    "VK_DRIVER_FILES",           // loader 1.3.207+: replaces driver discovery
    "VK_ICD_FILENAMES",          // its deprecated alias (LunarG setup-env still sets it)
    "VK_ADD_DRIVER_FILES",       // adds drivers ahead of the discovered ones
    "VK_LOADER_DRIVERS_SELECT",  // loader 1.3.234+: keep only manifests matching these globs
    "VK_LOADER_DRIVERS_DISABLE", // loader 1.3.234+: drop manifests matching these globs
};

} // namespace

DriverInfo query_driver_info(VkPhysicalDevice physical_device) {
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &driver;
    vkGetPhysicalDeviceProperties2(physical_device, &props);

    DriverInfo out;
    out.device_name = bounded(props.properties.deviceName);
    out.driver_name = bounded(driver.driverName);
    out.driver_info = bounded(driver.driverInfo);
    out.driver_id = driver.driverID;
    out.api_version = props.properties.apiVersion;
    out.driver_version = props.properties.driverVersion;
    return out;
}

std::vector<LoaderDriverOverride> loader_driver_overrides(const GetEnvFn& getenv) {
    std::vector<LoaderDriverOverride> out;
    if (!getenv) return out;
    for (const std::string_view name : k_driver_variables) {
        const char* value = getenv(name.data()); // the views name string literals
        if (value != nullptr && *value != '\0') out.push_back({name, value});
    }
    return out;
}

std::string describe_driver(const DriverInfo& driver,
                            const std::vector<LoaderDriverOverride>& overrides) {
    std::string line = std::format(
        "driver: {} {} (id {}, driverVersion 0x{:08x}, Vulkan {}.{}.{}) on {}",
        driver.driver_name.empty() ? std::string("<unnamed>") : driver.driver_name,
        driver.driver_info.empty() ? std::string("<no version info>") : driver.driver_info,
        static_cast<std::uint32_t>(driver.driver_id), driver.driver_version,
        VK_API_VERSION_MAJOR(driver.api_version), VK_API_VERSION_MINOR(driver.api_version),
        VK_API_VERSION_PATCH(driver.api_version), driver.device_name);
    if (overrides.empty()) {
        line += "; ICDs: loader default discovery";
    } else {
        for (const LoaderDriverOverride& o : overrides) {
            line += std::format("; {}={}", o.variable, o.value);
        }
    }
    return line;
}

} // namespace engine::rhi
