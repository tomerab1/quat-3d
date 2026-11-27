#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"

namespace engine::rhi {

// Which descriptor model the device was created with. ext_descriptor_buffer is
// the engine's mandated model (CLAUDE.md). classic_sets exists only as the
// ENGINE_DESCRIPTOR_SETS_FALLBACK compatibility path for drivers that do not
// implement VK_EXT_descriptor_buffer (MoltenVK on macOS); without that build
// flag a device lacking the extension is rejected and this is always
// ext_descriptor_buffer.
enum class DescriptorBackend : std::uint8_t { ext_descriptor_buffer, classic_sets };

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

    // Whether shaderStorageImageExtendedFormats is enabled (it is whenever the
    // device offers it): storage images in the extended formats (RG32F, R8,
    // ...) are only defined with it.
    [[nodiscard]] bool storage_image_extended_formats() const {
        return storage_image_extended_formats_ && !simulate_missing_extended_formats_;
    }
    // For passes whose shaders write storage images in extended `formats`:
    // the feature above plus VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT for each.
    // The error names `what` and the missing piece.
    [[nodiscard]] std::expected<void, core::Error>
    require_extended_storage_formats(std::span<const VkFormat> formats,
                                     std::string_view what) const;
    // Tests: behave as a device without the extended storage formats.
    void simulate_missing_extended_storage_formats(bool missing) {
        simulate_missing_extended_formats_ = missing;
    }

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
    DescriptorBackend        descriptor_backend_ = DescriptorBackend::ext_descriptor_buffer;
    bool                     storage_image_extended_formats_ = false;
    bool                     simulate_missing_extended_formats_ = false;
};

} // namespace engine::rhi
