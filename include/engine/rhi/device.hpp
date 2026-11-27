#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string_view>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/queue_selection.hpp"
#include "engine/rhi/texture_caps.hpp"

namespace engine::rhi {

// Which descriptor model the device was created with. ext_descriptor_buffer is
// the engine's mandated model (CLAUDE.md). classic_sets exists only as the
// ENGINE_DESCRIPTOR_SETS_FALLBACK compatibility path for drivers that do not
// implement VK_EXT_descriptor_buffer (MoltenVK on macOS); without that build
// flag a device lacking the extension is rejected and this is always
// ext_descriptor_buffer.
enum class DescriptorBackend : std::uint8_t { ext_descriptor_buffer, classic_sets };

// Creates the window surface for a freshly created instance (e.g. via
// SDL_Vulkan_CreateSurface). See Device::CreateInfo::create_surface.
using CreateSurfaceFn = std::function<std::expected<VkSurfaceKHR, core::Error>(VkInstance)>;

// Owns the core Vulkan objects: instance, (debug build) debug messenger, the
// window surface (when one was requested), the selected physical device, the
// logical device, and its queues. Move-only RAII wrapper — destroying it tears
// everything down in order.
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
        // Optional. Called once the instance exists and before a physical
        // device is chosen: only devices with a queue family that can present
        // to the returned surface are considered (a graphics family that also
        // presents is preferred), and present_queue() is valid. The device
        // owns the surface and destroys it before the instance. Leave empty
        // for a surface-less device (tools, tests): present_queue() is then
        // null and queue_families().present is VK_QUEUE_FAMILY_IGNORED.
        CreateSurfaceFn create_surface{};
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
    // The queue that presents to surface(): the graphics queue when its family
    // can present (the usual case), else one from queue_families().present.
    // Null for a surface-less device.
    [[nodiscard]] VkQueue present_queue() const { return present_queue_; }
    // The surface created through CreateInfo::create_surface (owned by the
    // device), or null.
    [[nodiscard]] VkSurfaceKHR surface() const { return surface_; }
    [[nodiscard]] const VkPhysicalDeviceProperties& properties() const { return properties_; }
    [[nodiscard]] const VkPhysicalDeviceDescriptorBufferPropertiesEXT&
    descriptor_buffer_properties() const { return descriptor_buffer_props_; }
    // Device limit for anisotropic filtering; 0 when samplerAnisotropy is
    // unsupported (the feature is enabled whenever the device offers it).
    [[nodiscard]] float max_sampler_anisotropy() const { return max_sampler_anisotropy_; }
    // Sampled-texture capabilities of this device (see TransferContext).
    [[nodiscard]] const TextureCaps& texture_caps() const {
        return texture_caps_;
    }
    [[nodiscard]] DescriptorBackend descriptor_backend() const { return descriptor_backend_; }
    [[nodiscard]] bool uses_descriptor_buffer() const {
        return descriptor_backend_ == DescriptorBackend::ext_descriptor_buffer;
    }

    // Checks that shader storage images with these declared formats (see
    // LoadedShader::storage_image_formats) can be used on this device: each
    // format must support VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT with optimal
    // tiling, and a storage image without a declared format
    // (VK_FORMAT_UNDEFINED) is refused — the engine does not rely on the
    // optional shaderStorageImage{Read,Write}WithoutFormat features. Called
    // at pipeline creation; `what` names the shader in the error.
    [[nodiscard]] std::expected<void, core::Error>
    check_storage_image_formats(std::span<const VkFormat> formats, std::string_view what) const;

private:
    void destroy() noexcept;

    VkInstance               instance_         = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_  = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_          = VK_NULL_HANDLE;
    VkPhysicalDevice         physical_device_  = VK_NULL_HANDLE;
    VkDevice                 device_           = VK_NULL_HANDLE;
    QueueFamilies            queue_families_{};
    VkQueue                  graphics_queue_   = VK_NULL_HANDLE;
    VkQueue                  compute_queue_    = VK_NULL_HANDLE;
    VkQueue                  transfer_queue_   = VK_NULL_HANDLE;
    VkQueue                  present_queue_    = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_buffer_props_{};
    float                    max_sampler_anisotropy_ = 0.0F;
    TextureCaps              texture_caps_{};
    DescriptorBackend        descriptor_backend_ = DescriptorBackend::ext_descriptor_buffer;
};

} // namespace engine::rhi
