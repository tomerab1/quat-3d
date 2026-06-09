#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"

namespace engine::rhi {

// Queue family indices selected for the logical device. Compute and transfer
// fall back to the graphics family when no dedicated family exists.
struct QueueFamilies {
    std::uint32_t graphics = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t compute  = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t transfer = VK_QUEUE_FAMILY_IGNORED;
};

// Owns the core Vulkan objects: instance, (debug build) debug messenger,
// the selected physical device, the logical device, and the three queues.
// Move-only RAII wrapper — destroying it tears everything down in order.
class Device {
public:
    struct CreateInfo {
        // Instance extensions required by the windowing system (from
        // SDL_Vulkan_GetInstanceExtensions). The device adds debug-utils itself
        // when validation is enabled.
        std::span<const char* const> instance_extensions{};
        // Enable VK_LAYER_KHRONOS_validation + debug messenger. Caller usually
        // passes true in debug builds, false in release.
        bool enable_validation = false;
        const char* application_name = "quat-3d";
    };

    [[nodiscard]] static std::expected<Device, core::Error> create(const CreateInfo& info);

    Device() = default;
    ~Device();

    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] VkInstance instance() const { return instance_; }
    [[nodiscard]] VkPhysicalDevice physical_device() const { return physical_device_; }
    [[nodiscard]] VkDevice handle() const { return device_; }
    [[nodiscard]] const QueueFamilies& queue_families() const { return queue_families_; }
    [[nodiscard]] VkQueue graphics_queue() const { return graphics_queue_; }
    [[nodiscard]] VkQueue compute_queue() const { return compute_queue_; }
    [[nodiscard]] VkQueue transfer_queue() const { return transfer_queue_; }
    [[nodiscard]] const VkPhysicalDeviceProperties& properties() const { return properties_; }
    [[nodiscard]] const VkPhysicalDeviceDescriptorBufferPropertiesEXT&
    descriptor_buffer_properties() const { return descriptor_buffer_props_; }
    // Device limit for anisotropic filtering; 0 when samplerAnisotropy is
    // unsupported (the feature is enabled whenever the device offers it).
    [[nodiscard]] float max_sampler_anisotropy() const { return max_sampler_anisotropy_; }

private:
    void destroy() noexcept;

    VkInstance               instance_         = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_  = VK_NULL_HANDLE;
    VkPhysicalDevice         physical_device_  = VK_NULL_HANDLE;
    VkDevice                 device_           = VK_NULL_HANDLE;
    QueueFamilies            queue_families_{};
    VkQueue                  graphics_queue_   = VK_NULL_HANDLE;
    VkQueue                  compute_queue_    = VK_NULL_HANDLE;
    VkQueue                  transfer_queue_   = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_buffer_props_{};
    float                    max_sampler_anisotropy_ = 0.0F;
};

} // namespace engine::rhi
