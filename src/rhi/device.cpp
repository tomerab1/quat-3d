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
// synchronization2 are core in Vulkan 1.3; swapchain is foundational for
// presentation. descriptor_buffer is the mandated descriptor model (CLAUDE.md);
// it is hard-required unless ENGINE_DESCRIPTOR_SETS_FALLBACK permits the
// classic-sets compatibility path on drivers that lack it (MoltenVK on macOS).

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
    // VK_EXT_descriptor_buffer feature + extension both present. Always true
    // unless ENGINE_DESCRIPTOR_SETS_FALLBACK admits classic-sets devices.
    bool has_descriptor_buffer = false;
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
        VkPhysicalDeviceDescriptorBufferFeaturesEXT db_features{};
        db_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.pNext = &db_features;
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.pNext = &features12;
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
        if (!features12.bufferDeviceAddress) continue;

        const auto exts = device_extensions(device);
        if (!has_extension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        const bool db_ok = db_features.descriptorBuffer == VK_TRUE
            && has_extension(exts, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
#ifndef ENGINE_DESCRIPTOR_SETS_FALLBACK
        if (!db_ok) continue;
#endif

        const QueueSelection queues = select_queue_families(device);
        if (!queues.valid) continue;

        // +1 tie-breaker: prefer a descriptor-buffer-capable device over an
        // otherwise equal one that would force the classic-sets fallback.
        const int score = device_type_score(props.deviceType) + (db_ok ? 1 : 0);
        if (score > best.score) {
            best = Candidate{device, queues.families, props, score, db_ok};
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

#ifdef ENGINE_DESCRIPTOR_SETS_FALLBACK
    // Portability (non-conformant) ICDs such as MoltenVK are only enumerated
    // when the instance opts in via VK_KHR_portability_enumeration; without it
    // vkCreateInstance fails with VK_ERROR_INCOMPATIBLE_DRIVER on macOS.
    bool enumerate_portability = false;
    {
        std::uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        enumerate_portability = has_extension(
            available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
    if (enumerate_portability) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
#endif

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
#ifdef ENGINE_DESCRIPTOR_SETS_FALLBACK
    if (enumerate_portability) {
        instance_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

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
        return fail("no Vulkan 1.3 device with dynamic_rendering + synchronization2 "
                    "+ swapchain + descriptor_buffer found");
    }
    dev.physical_device_ = candidate.device;
    dev.queue_families_  = candidate.families;
    dev.properties_      = candidate.properties;
    dev.descriptor_backend_ = candidate.has_descriptor_buffer
        ? DescriptorBackend::ext_descriptor_buffer
        : DescriptorBackend::classic_sets;

    std::fprintf(stderr, "[rhi] selected GPU: %s (Vulkan %u.%u.%u, queues g=%u c=%u t=%u, "
                 "descriptors: %s)\n",
                 candidate.properties.deviceName,
                 VK_API_VERSION_MAJOR(candidate.properties.apiVersion),
                 VK_API_VERSION_MINOR(candidate.properties.apiVersion),
                 VK_API_VERSION_PATCH(candidate.properties.apiVersion),
                 candidate.families.graphics, candidate.families.compute,
                 candidate.families.transfer,
                 candidate.has_descriptor_buffer ? "descriptor_buffer" : "classic sets (fallback)");

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

    VkPhysicalDeviceDescriptorBufferFeaturesEXT db_features{};
    db_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    db_features.descriptorBuffer = VK_TRUE;

    // Anisotropic filtering is optional (lavapipe gained it only recently) —
    // enable it when offered and record the limit so samplers can use it.
    VkPhysicalDeviceFeatures supported_core{};
    vkGetPhysicalDeviceFeatures(dev.physical_device_, &supported_core);
    VkPhysicalDeviceFeatures core_features{};
    core_features.samplerAnisotropy = supported_core.samplerAnisotropy;
    if (supported_core.samplerAnisotropy == VK_TRUE) {
        dev.max_sampler_anisotropy_ = candidate.properties.limits.maxSamplerAnisotropy;
    }

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress = VK_TRUE;
    // Only chain the descriptor-buffer feature struct when the extension is
    // actually enabled; requesting it otherwise is a spec violation.
    features12.pNext = candidate.has_descriptor_buffer ? &db_features : nullptr;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    features13.pNext = &features12;

    std::vector<const char*> device_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (candidate.has_descriptor_buffer) {
        device_exts.push_back(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    }
#ifdef ENGINE_DESCRIPTOR_SETS_FALLBACK
    // Portability-subset ICDs advertise VK_KHR_portability_subset, and the spec
    // requires it to be enabled whenever present. String spelled out to avoid
    // pulling in vulkan_beta.h.
    const auto available_device_exts = device_extensions(dev.physical_device_);
    if (has_extension(available_device_exts, "VK_KHR_portability_subset")) {
        device_exts.push_back("VK_KHR_portability_subset");
    }
#endif

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &features13;
    device_info.queueCreateInfoCount = static_cast<std::uint32_t>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_exts.size());
    device_info.ppEnabledExtensionNames = device_exts.data();
    device_info.pEnabledFeatures = &core_features;

    if (VkResult r = vkCreateDevice(dev.physical_device_, &device_info, nullptr, &dev.device_);
        r != VK_SUCCESS) {
        return fail("vkCreateDevice failed", r);
    }

    vkGetDeviceQueue(dev.device_, candidate.families.graphics, 0, &dev.graphics_queue_);
    vkGetDeviceQueue(dev.device_, candidate.families.compute, 0, &dev.compute_queue_);
    vkGetDeviceQueue(dev.device_, candidate.families.transfer, 0, &dev.transfer_queue_);

    // Descriptor buffer sizes/alignments, needed when laying out descriptor
    // buffers (Slice 2.3). pNext is cleared so the cached struct is self-contained.
    // Skipped on the classic-sets fallback backend: the extension is absent and
    // the struct stays zeroed (nothing reads it on that path).
    if (candidate.has_descriptor_buffer) {
        dev.descriptor_buffer_props_.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &dev.descriptor_buffer_props_;
        vkGetPhysicalDeviceProperties2(dev.physical_device_, &props2);
        dev.descriptor_buffer_props_.pNext = nullptr;
    }

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
        descriptor_buffer_props_ = other.descriptor_buffer_props_;
        max_sampler_anisotropy_  = other.max_sampler_anisotropy_;
        descriptor_backend_      = other.descriptor_backend_;
    }
    return *this;
}

} // namespace engine::rhi
