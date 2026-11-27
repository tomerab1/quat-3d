#include "engine/rhi/device.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "engine/core/log.hpp"

namespace engine::rhi {

namespace {

constexpr const char* k_validation_layer = "VK_LAYER_KHRONOS_validation";

// Device extensions every selected GPU must support. dynamic_rendering and
// synchronization2 are core in Vulkan 1.3; swapchain is required (and enabled)
// when the device presents to a surface. descriptor_buffer is the mandated
// descriptor model (CLAUDE.md); it is hard-required unless
// ENGINE_DESCRIPTOR_SETS_FALLBACK permits the classic-sets compatibility path
// on drivers that lack it (MoltenVK on macOS).

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
    // Routed into the engine log ring (category "vulkan") so remote debugging
    // (debug.log) sees validation messages; log() also echoes to stderr.
    core::LogLevel level = core::LogLevel::info;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)        level = core::LogLevel::error;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) level = core::LogLevel::warn;
    core::log(level, "vulkan", "{}", data != nullptr && data->pMessage != nullptr
                                         ? data->pMessage
                                         : "(no message)");
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

[[nodiscard]] std::vector<VkQueueFamilyProperties> queue_family_properties(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props.data());
    return props;
}

// A surface the device can present to must also report at least one format
// and present mode, or no swapchain can be created for it.
[[nodiscard]] bool surface_has_formats_and_modes(VkPhysicalDevice device, VkSurfaceKHR surface) {
    std::uint32_t formats = 0;
    std::uint32_t modes = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formats, nullptr) != VK_SUCCESS ||
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &modes, nullptr) !=
            VK_SUCCESS) {
        return false;
    }
    return formats > 0 && modes > 0;
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

// PCI vendor id Apple GPUs report through MoltenVK / KosmicKrisp.
constexpr std::uint32_t k_vendor_apple = 0x106B;

// Require Vulkan 1.3 + dynamicRendering + synchronization2 (and, with a
// surface, VK_KHR_swapchain plus a queue family that can present to it), then
// rank by device type (prefer discrete). On a multi-GPU machine whose display
// GPU is not the discrete one, presentation support decides. Each rejected
// device is noted in `rejected` ("name: reason; ...") for the error message.
[[nodiscard]] Candidate pick_physical_device(VkInstance instance, VkSurfaceKHR surface,
                                             std::string& rejected) {
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

        const auto reject = [&](const char* why) {
            rejected += std::format("{}{}: {}", rejected.empty() ? "" : "; ",
                                    props.deviceName, why);
        };

        if (VK_API_VERSION_MAJOR(props.apiVersion) < 1 ||
            VK_API_VERSION_MINOR(props.apiVersion) < 3) {
            reject("Vulkan 1.3 not supported");
            continue;
        }
        if (!features13.dynamicRendering || !features13.synchronization2) {
            reject("no dynamicRendering / synchronization2");
            continue;
        }
        if (!features12.bufferDeviceAddress) {
            reject("no bufferDeviceAddress");
            continue;
        }

        const auto exts = device_extensions(device);
        if (surface != VK_NULL_HANDLE && !has_extension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            reject("no VK_KHR_swapchain");
            continue;
        }

        const bool db_ok = db_features.descriptorBuffer == VK_TRUE
            && has_extension(exts, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
#ifndef ENGINE_DESCRIPTOR_SETS_FALLBACK
        if (!db_ok) {
            reject("no VK_EXT_descriptor_buffer");
            continue;
        }
#endif

        const auto family_props = queue_family_properties(device);
        PresentSupportFn can_present{};
        if (surface != VK_NULL_HANDLE) {
            can_present = [device, surface](std::uint32_t family) {
                VkBool32 supported = VK_FALSE;
                return vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface,
                                                            &supported) == VK_SUCCESS &&
                       supported == VK_TRUE;
            };
        }
        const std::optional<QueueFamilies> families =
            select_queue_families(family_props, can_present);
        if (!families) {
            reject(surface != VK_NULL_HANDLE ? "no graphics queue, or no queue that can "
                                               "present to the window surface"
                                             : "no graphics queue");
            continue;
        }
        if (surface != VK_NULL_HANDLE && !surface_has_formats_and_modes(device, surface)) {
            reject("the window surface reports no formats or present modes");
            continue;
        }

        // Tie-breakers within a device type: prefer a descriptor-buffer-capable
        // device over one that would force the classic-sets fallback (+2),
        // then one whose graphics queue presents itself (+1).
        const bool presents_from_graphics =
            families->present != VK_QUEUE_FAMILY_IGNORED && families->present == families->graphics;
        const int score = device_type_score(props.deviceType) + (db_ok ? 2 : 0) +
                          (presents_from_graphics ? 1 : 0);
        if (score > best.score) {
            best = Candidate{device, *families, props, score, db_ok};
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

    // ---- Window surface -------------------------------------------------
    // Created before the physical device is chosen so selection can require
    // presentation support (owned by `dev` from here: destroyed on failure).
    if (info.create_surface) {
        auto surface = info.create_surface(dev.instance_);
        if (!surface) return std::unexpected(surface.error());
        if (*surface == VK_NULL_HANDLE) return fail("create_surface returned a null surface");
        dev.surface_ = *surface;
        dev.presentable_ = true; // VK_KHR_swapchain is required and enabled below
    }

    // ---- Physical device ------------------------------------------------
    std::string rejected;
    const Candidate candidate = pick_physical_device(dev.instance_, dev.surface_, rejected);
    if (candidate.device == VK_NULL_HANDLE) {
        return fail(std::format(
            "no suitable Vulkan device: need Vulkan 1.3 with dynamic_rendering + "
            "synchronization2 + descriptor_buffer{} ({})",
            dev.surface_ != VK_NULL_HANDLE
                ? " + swapchain and a queue that can present to the window surface"
                : "",
            rejected.empty() ? "no Vulkan devices found" : rejected));
    }
    dev.physical_device_ = candidate.device;
    dev.queue_families_  = candidate.families;
    dev.properties_      = candidate.properties;
    dev.descriptor_backend_ = candidate.has_descriptor_buffer
        ? DescriptorBackend::ext_descriptor_buffer
        : DescriptorBackend::classic_sets;

    const std::string present_family =
        candidate.families.present == VK_QUEUE_FAMILY_IGNORED
            ? std::string("none")
            : std::to_string(candidate.families.present);
    std::fprintf(stderr, "[rhi] selected GPU: %s (Vulkan %u.%u.%u, queues g=%u c=%u t=%u p=%s, "
                 "descriptors: %s)\n",
                 candidate.properties.deviceName,
                 VK_API_VERSION_MAJOR(candidate.properties.apiVersion),
                 VK_API_VERSION_MINOR(candidate.properties.apiVersion),
                 VK_API_VERSION_PATCH(candidate.properties.apiVersion),
                 candidate.families.graphics, candidate.families.compute,
                 candidate.families.transfer, present_family.c_str(),
                 candidate.has_descriptor_buffer ? "descriptor_buffer" : "classic sets (fallback)");

    // ---- Logical device -------------------------------------------------
    std::set<std::uint32_t> unique_families = {
        candidate.families.graphics, candidate.families.compute, candidate.families.transfer};
    if (candidate.families.present != VK_QUEUE_FAMILY_IGNORED) {
        unique_families.insert(candidate.families.present);
    }

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
    // Storage images in RG32F / R8 (Hi-Z pyramid, GTAO edges) need the
    // extended storage formats; universally offered on desktop and MoltenVK.
    core_features.shaderStorageImageExtendedFormats =
        supported_core.shaderStorageImageExtendedFormats;
    dev.storage_image_extended_formats_ =
        supported_core.shaderStorageImageExtendedFormats == VK_TRUE;
    // Shadow pancaking (casters before a cascade's near plane clamp to depth
    // 0) keeps cascade depth ranges tight.
    core_features.depthClamp = supported_core.depthClamp;
    dev.depth_clamp_ = supported_core.depthClamp == VK_TRUE;
    if (supported_core.samplerAnisotropy == VK_TRUE) {
        dev.max_sampler_anisotropy_ = candidate.properties.limits.maxSamplerAnisotropy;
    }
    // Block-compressed texture families, likewise enabled whenever offered;
    // asset loads pick their transcode targets from what is enabled here.
    core_features.textureCompressionBC = supported_core.textureCompressionBC;
    core_features.textureCompressionASTC_LDR = supported_core.textureCompressionASTC_LDR;
    dev.texture_caps_.bc = supported_core.textureCompressionBC == VK_TRUE;
    dev.texture_caps_.astc_ldr = supported_core.textureCompressionASTC_LDR == VK_TRUE;
    dev.texture_caps_.prefer_astc =
        dev.texture_caps_.astc_ldr && candidate.properties.vendorID == k_vendor_apple;
    dev.texture_caps_.max_dimension_2d = candidate.properties.limits.maxImageDimension2D;

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

    // VK_KHR_swapchain requires the instance's VK_KHR_surface, so a
    // surface-less device (tools, tests) does not enable it.
    std::vector<const char*> device_exts;
    if (dev.surface_ != VK_NULL_HANDLE) {
        device_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
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
    if (candidate.families.present != VK_QUEUE_FAMILY_IGNORED) {
        vkGetDeviceQueue(dev.device_, candidate.families.present, 0, &dev.present_queue_);
    }

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

std::expected<void, core::Error>
Device::check_storage_image_formats(std::span<const VkFormat> formats,
                                    std::string_view what) const {
    for (const VkFormat format : formats) {
        if (format == VK_FORMAT_UNDEFINED) {
            return std::unexpected(core::Error{
                std::string(what) + ": a storage image is declared without a format "
                                    "(declare [format(...)] on the RWTexture)"});
        }
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physical_device_, format, &props);
        if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0) {
            return std::unexpected(core::Error{std::string(what) + ": storage image format " +
                                               std::to_string(static_cast<int>(format)) +
                                               " is not supported for storage on this device"});
        }
    }
    return {};
}

std::expected<void, core::Error>
Device::require_extended_storage_formats(std::span<const VkFormat> formats,
                                         std::string_view what) const {
    if (!storage_image_extended_formats()) {
        return std::unexpected(core::Error{
            std::string(what) +
            ": the device lacks shaderStorageImageExtendedFormats (needed for its storage "
            "image formats)"});
    }
    for (const VkFormat format : formats) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physical_device_, format, &props);
        if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0) {
            return std::unexpected(core::Error{std::string(what) + ": storage image format " +
                                               std::to_string(static_cast<int>(format)) +
                                               " is not supported for storage on this device"});
        }
    }
    return {};
}

void Device::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    // Any swapchain on the surface is gone by now (it needed the device).
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
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
    graphics_queue_ = compute_queue_ = transfer_queue_ = present_queue_ = VK_NULL_HANDLE;
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
        surface_         = std::exchange(other.surface_, VK_NULL_HANDLE);
        physical_device_ = std::exchange(other.physical_device_, VK_NULL_HANDLE);
        device_          = std::exchange(other.device_, VK_NULL_HANDLE);
        queue_families_  = other.queue_families_;
        graphics_queue_  = std::exchange(other.graphics_queue_, VK_NULL_HANDLE);
        compute_queue_   = std::exchange(other.compute_queue_, VK_NULL_HANDLE);
        transfer_queue_  = std::exchange(other.transfer_queue_, VK_NULL_HANDLE);
        present_queue_   = std::exchange(other.present_queue_, VK_NULL_HANDLE);
        properties_     = other.properties_;
        descriptor_buffer_props_ = other.descriptor_buffer_props_;
        max_sampler_anisotropy_  = other.max_sampler_anisotropy_;
        texture_caps_            = other.texture_caps_;
        descriptor_backend_      = other.descriptor_backend_;
        presentable_             = std::exchange(other.presentable_, false);
        storage_image_extended_formats_ = other.storage_image_extended_formats_;
        simulate_missing_extended_formats_ = other.simulate_missing_extended_formats_;
        depth_clamp_ = other.depth_clamp_;
        simulate_missing_depth_clamp_ = other.simulate_missing_depth_clamp_;
    }
    return *this;
}

} // namespace engine::rhi
