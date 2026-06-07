#include "engine/rhi/device.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <set>
#include <vector>

namespace engine::rhi {

namespace {

constexpr const char* k_validation_layer = "VK_LAYER_KHRONOS_validation";

// Device extensions every selected GPU must support. dynamic_rendering and
// synchronization2 are core in Vulkan 1.3, so only the swapchain extension is
// listed here — it is foundational for presentation (Slice 1.4) and is enabled
// at device-creation time.
constexpr std::array<const char*, 1> k_required_device_extensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

[[nodiscard]] std::string vk_result_string(VkResult result) {
    switch (result) {
    case VK_SUCCESS:                    return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:   return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:    return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:  return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:  return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default:                            return std::format("VkResult({})", static_cast<int>(result));
    }
}

[[nodiscard]] std::unexpected<core::Error> fail(std::string what, VkResult result) {
    return std::unexpected(core::Error{std::format("{}: {}", what, vk_result_string(result))});
}

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/) {
    const char* level = "INFO";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)        level = "ERROR";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) level = "WARN";
    std::fprintf(stderr, "[vulkan][%s] %s\n", level, data->pMessage);
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT make_debug_messenger_info() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debug_callback;
    return info;
}

[[nodiscard]] bool validation_layer_available() {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& l) {
        return std::strcmp(l.layerName, k_validation_layer) == 0;
    });
}

[[nodiscard]] std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, exts.data());
    return exts;
}

[[nodiscard]] bool has_extension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(), [name](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

// Queue family selection result with a validity flag.
struct QueueSelection {
    QueueFamilies families{};
    bool valid = false;
};

[[nodiscard]] QueueSelection select_queue_families(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props.data());

    constexpr std::uint32_t none = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t graphics = none, compute = none, transfer = none;
    std::uint32_t dedicated_compute = none, dedicated_transfer = none;

    for (std::uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = props[i].queueFlags;
        const bool g = flags & VK_QUEUE_GRAPHICS_BIT;
        const bool c = flags & VK_QUEUE_COMPUTE_BIT;
        const bool t = flags & VK_QUEUE_TRANSFER_BIT;

        if (g && graphics == none) graphics = i;
        if (c && compute == none) compute = i;
        if (t && transfer == none) transfer = i;
        // Prefer a compute family without graphics (async compute).
        if (c && !g && dedicated_compute == none) dedicated_compute = i;
        // Prefer a transfer-only family (dedicated DMA).
        if (t && !g && !c && dedicated_transfer == none) dedicated_transfer = i;
    }

    if (graphics == none) return {};
    if (dedicated_compute != none)  compute = dedicated_compute;
    if (dedicated_transfer != none) transfer = dedicated_transfer;
    // Graphics queues always support transfer; ensure a fallback.
    if (compute == none)  compute = graphics;
    if (transfer == none) transfer = graphics;

    return QueueSelection{QueueFamilies{graphics, compute, transfer}, true};
}

[[nodiscard]] int device_type_score(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 1000;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 500;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 250;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 100;
    default:                                     return 50;
    }
}

struct Candidate {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    QueueFamilies families{};
    VkPhysicalDeviceProperties properties{};
    int score = -1;
};

// Require Vulkan 1.3 + dynamicRendering + synchronization2 + swapchain, then
// rank by device type (prefer discrete). In this WSL2 box only llvmpipe
// qualifies; on real hardware this picks the discrete GPU. See CLAUDE.md.
[[nodiscard]] Candidate pick_physical_device(VkInstance instance) {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    Candidate best{};
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(device, &features2);

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);

        if (VK_API_VERSION_MAJOR(props.apiVersion) < 1 ||
            VK_API_VERSION_MINOR(props.apiVersion) < 3) {
            continue;
        }
        if (!features13.dynamicRendering || !features13.synchronization2) continue;

        const auto exts = device_extensions(device);
        const bool all_exts = std::all_of(
            k_required_device_extensions.begin(), k_required_device_extensions.end(),
            [&](const char* name) { return has_extension(exts, name); });
        if (!all_exts) continue;

        const QueueSelection queues = select_queue_families(device);
        if (!queues.valid) continue;

        const int score = device_type_score(props.deviceType);
        if (score > best.score) {
            best = Candidate{device, queues.families, props, score};
        }
    }
    return best;
}

} // namespace

std::expected<Device, core::Error> Device::create(const CreateInfo& info) {
    Device dev;

    const bool want_validation = info.enable_validation && validation_layer_available();

    // ---- Instance -------------------------------------------------------
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = info.application_name;
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = "quat-3d";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions(info.instance_extensions.begin(),
                                        info.instance_extensions.end());
    std::vector<const char*> layers;
    if (want_validation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back(k_validation_layer);
    }

    VkDebugUtilsMessengerCreateInfoEXT debug_info = make_debug_messenger_info();

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    if (want_validation) {
        instance_info.pNext = &debug_info; // also covers create/destroy messages
    }

    if (VkResult r = vkCreateInstance(&instance_info, nullptr, &dev.instance_); r != VK_SUCCESS) {
        return fail("vkCreateInstance failed", r);
    }

    // ---- Debug messenger ------------------------------------------------
    if (want_validation) {
        auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(dev.instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger != nullptr) {
            create_messenger(dev.instance_, &debug_info, nullptr, &dev.debug_messenger_);
        }
    }

    // ---- Physical device ------------------------------------------------
    const Candidate candidate = pick_physical_device(dev.instance_);
    if (candidate.device == VK_NULL_HANDLE) {
        return fail("no Vulkan 1.3 device with dynamic_rendering + synchronization2 + swapchain found");
    }
    dev.physical_device_ = candidate.device;
    dev.queue_families_  = candidate.families;
    dev.properties_      = candidate.properties;

    std::fprintf(stderr, "[rhi] selected GPU: %s (Vulkan %u.%u.%u, queues g=%u c=%u t=%u)\n",
                 candidate.properties.deviceName,
                 VK_API_VERSION_MAJOR(candidate.properties.apiVersion),
                 VK_API_VERSION_MINOR(candidate.properties.apiVersion),
                 VK_API_VERSION_PATCH(candidate.properties.apiVersion),
                 candidate.families.graphics, candidate.families.compute,
                 candidate.families.transfer);

    // ---- Logical device -------------------------------------------------
    const std::set<std::uint32_t> unique_families = {
        candidate.families.graphics, candidate.families.compute, candidate.families.transfer};

    const float priority = 1.0F;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    queue_infos.reserve(unique_families.size());
    for (std::uint32_t family : unique_families) {
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queue_infos.push_back(qi);
    }

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &features13;
    device_info.queueCreateInfoCount = static_cast<std::uint32_t>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount =
        static_cast<std::uint32_t>(k_required_device_extensions.size());
    device_info.ppEnabledExtensionNames = k_required_device_extensions.data();

    if (VkResult r = vkCreateDevice(dev.physical_device_, &device_info, nullptr, &dev.device_);
        r != VK_SUCCESS) {
        return fail("vkCreateDevice failed", r);
    }

    vkGetDeviceQueue(dev.device_, candidate.families.graphics, 0, &dev.graphics_queue_);
    vkGetDeviceQueue(dev.device_, candidate.families.compute, 0, &dev.compute_queue_);
    vkGetDeviceQueue(dev.device_, candidate.families.transfer, 0, &dev.transfer_queue_);

    return dev;
}

void Device::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (debug_messenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_messenger != nullptr) {
            destroy_messenger(instance_, debug_messenger_, nullptr);
        }
        debug_messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    physical_device_ = VK_NULL_HANDLE;
    graphics_queue_ = compute_queue_ = transfer_queue_ = VK_NULL_HANDLE;
}

Device::~Device() {
    destroy();
}

Device::Device(Device&& other) noexcept {
    *this = std::move(other);
}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        destroy();
        instance_        = std::exchange(other.instance_, VK_NULL_HANDLE);
        debug_messenger_ = std::exchange(other.debug_messenger_, VK_NULL_HANDLE);
        physical_device_ = std::exchange(other.physical_device_, VK_NULL_HANDLE);
        device_          = std::exchange(other.device_, VK_NULL_HANDLE);
        queue_families_  = other.queue_families_;
        graphics_queue_  = std::exchange(other.graphics_queue_, VK_NULL_HANDLE);
        compute_queue_   = std::exchange(other.compute_queue_, VK_NULL_HANDLE);
        transfer_queue_  = std::exchange(other.transfer_queue_, VK_NULL_HANDLE);
        properties_      = other.properties_;
    }
    return *this;
}

} // namespace engine::rhi
