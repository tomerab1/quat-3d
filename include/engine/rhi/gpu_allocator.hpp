#pragma once

#include <cstdint>
#include <expected>
#ifdef ENGINE_DESCRIPTOR_SETS_FALLBACK
#include <optional>
#endif

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "engine/core/error.hpp"

namespace engine::rhi {

class Device;

// Move-only RAII wrapper over a VMA-backed VkBuffer. Per CLAUDE.md, all GPU
// allocations go through GpuAllocator; raw VkBuffer/VkDeviceMemory are never
// stored elsewhere.
class GpuBuffer {
public:
    GpuBuffer() = default;
    ~GpuBuffer();
    GpuBuffer(GpuBuffer&& other) noexcept;
    GpuBuffer& operator=(GpuBuffer&& other) noexcept;
    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;

    [[nodiscard]] VkBuffer handle() const { return buffer_; }
    [[nodiscard]] VkDeviceSize size() const { return size_; }
    // Non-null only when the buffer was created persistently mapped.
    [[nodiscard]] void* mapped() const { return mapped_; }

private:
    friend class GpuAllocator;
    void destroy() noexcept;

    VmaAllocator  allocator_  = nullptr;     // non-owning
    VkBuffer      buffer_     = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkDeviceSize  size_       = 0;
    void*         mapped_     = nullptr;
};

// Move-only RAII wrapper over a VMA-backed VkImage (no view — views are created
// by the consumer, e.g. the swapchain or render targets).
class GpuImage {
public:
    GpuImage() = default;
    ~GpuImage();
    GpuImage(GpuImage&& other) noexcept;
    GpuImage& operator=(GpuImage&& other) noexcept;
    GpuImage(const GpuImage&) = delete;
    GpuImage& operator=(const GpuImage&) = delete;

    [[nodiscard]] VkImage handle() const { return image_; }
    [[nodiscard]] VkFormat format() const { return format_; }
    [[nodiscard]] VkExtent3D extent() const { return extent_; }

private:
    friend class GpuAllocator;
    void destroy() noexcept;

    VmaAllocator  allocator_  = nullptr;     // non-owning
    VkImage       image_      = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkFormat      format_     = VK_FORMAT_UNDEFINED;
    VkExtent3D    extent_{};
};

// Move-only RAII wrapper over a VkImageView. Unlike GpuImage the view is a plain
// device object (no VMA allocation); it borrows the VkImage it is created from.
class ImageView {
public:
    ImageView() = default;
    ImageView(VkDevice device, VkImageView view) : device_(device), view_(view) {}
    ~ImageView();
    ImageView(ImageView&& other) noexcept;
    ImageView& operator=(ImageView&& other) noexcept;
    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;

    [[nodiscard]] VkImageView handle() const { return view_; }

private:
    void destroy() noexcept;

    VkDevice    device_ = VK_NULL_HANDLE;     // non-owning
    VkImageView view_   = VK_NULL_HANDLE;
};

// Move-only RAII wrapper over a VkSampler.
class Sampler {
public:
    Sampler() = default;
    Sampler(VkDevice device, VkSampler sampler) : device_(device), sampler_(sampler) {}
    ~Sampler();
    Sampler(Sampler&& other) noexcept;
    Sampler& operator=(Sampler&& other) noexcept;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    [[nodiscard]] VkSampler handle() const { return sampler_; }

private:
    void destroy() noexcept;

    VkDevice  device_  = VK_NULL_HANDLE;       // non-owning
    VkSampler sampler_ = VK_NULL_HANDLE;
};

// Owns the VmaAllocator and is the single entry point for GPU allocations.
class GpuAllocator {
public:
    [[nodiscard]] static std::expected<GpuAllocator, core::Error> create(const Device& device);

    GpuAllocator() = default;
    ~GpuAllocator();
    GpuAllocator(GpuAllocator&& other) noexcept;
    GpuAllocator& operator=(GpuAllocator&& other) noexcept;
    GpuAllocator(const GpuAllocator&) = delete;
    GpuAllocator& operator=(const GpuAllocator&) = delete;

    [[nodiscard]] VmaAllocator handle() const { return allocator_; }

    [[nodiscard]] std::expected<GpuBuffer, core::Error>
    create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                  VmaMemoryUsage memory_usage,
                  VmaAllocationCreateFlags flags = 0);

    // Host-visible, persistently mapped staging/upload buffer.
    [[nodiscard]] std::expected<GpuBuffer, core::Error>
    create_staging_buffer(VkDeviceSize size, VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

    // `array_layers`/`flags` support cubemaps (6 layers +
    // VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) and other layered images.
    [[nodiscard]] std::expected<GpuImage, core::Error>
    create_image(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                 std::uint32_t mip_levels = 1, std::uint32_t array_layers = 1,
                 VkImageCreateFlags flags = 0);

private:
    VmaAllocator allocator_ = nullptr;
};

// Immutable transfer context (transfer-capable queue + a command pool created on
// its family) used by the staging upload helper. `max_anisotropy` carries the
// device's sampler anisotropy limit (0 = feature unavailable) so asset uploads
// can build properly filtered samplers without a Device reference.
struct TransferContext {
    VkDevice      device         = VK_NULL_HANDLE;
    VkCommandPool pool           = VK_NULL_HANDLE;
    VkQueue       queue          = VK_NULL_HANDLE;
    float         max_anisotropy = 0.0F;
};

// Creates a device-local buffer and uploads `data` into it through a temporary
// staging buffer (blocking copy via the transfer context). The returned buffer
// always has TRANSFER_DST added to `usage`.
[[nodiscard]] std::expected<GpuBuffer, core::Error>
upload_device_buffer(GpuAllocator& allocator, const TransferContext& transfer,
                     const void* data, VkDeviceSize size, VkBufferUsageFlags usage);

// Device address of a buffer created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS.
// Required to reference uniform/storage buffers from a descriptor buffer.
[[nodiscard]] VkDeviceAddress
buffer_device_address(VkDevice device, const GpuBuffer& buffer);

#ifdef ENGINE_DESCRIPTOR_SETS_FALLBACK
// Classic-sets fallback support: vkUpdateDescriptorSets needs VkBuffer + offset,
// but the descriptor API traffics in raw device addresses. Buffers created with
// SHADER_DEVICE_ADDRESS usage are recorded so a write can be translated back to
// the buffer that contains the address.
struct BufferAddressRange {
    VkBuffer     buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
};
[[nodiscard]] std::optional<BufferAddressRange>
find_buffer_for_address(VkDeviceAddress address);
#endif

// Full mip chain length for an extent: floor(log2(max(w, h))) + 1.
[[nodiscard]] std::uint32_t full_mip_levels(VkExtent2D extent);

// Creates a device-local, optimally-tiled image and uploads `data` (tightly
// packed pixels matching `format` and `extent`) through a staging buffer. When
// `mip_levels` > 1 the remaining levels are generated with a vkCmdBlitImage
// downsample chain (the format must support linear blits). The image is left in
// VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL (all levels); SAMPLED and
// TRANSFER_DST are always added to `usage`.
[[nodiscard]] std::expected<GpuImage, core::Error>
upload_device_image(GpuAllocator& allocator, const TransferContext& transfer,
                    const void* data, VkDeviceSize size, VkExtent2D extent,
                    VkFormat format,
                    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                    std::uint32_t mip_levels = 1);

// Creates a VkImageView over `image` wrapped in an RAII ImageView. Defaults to a
// 2D colour view of mip 0; the extra parameters cover cube views
// (VK_IMAGE_VIEW_TYPE_CUBE, layer_count = 6), single-mip array views for storage
// writes (VK_IMAGE_VIEW_TYPE_2D_ARRAY, base_mip = mip, layer_count = 6), etc.
[[nodiscard]] std::expected<ImageView, core::Error>
create_image_view(VkDevice device, VkImage image, VkFormat format,
                  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                  std::uint32_t mip_levels = 1,
                  VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D,
                  std::uint32_t base_mip = 0, std::uint32_t base_layer = 0,
                  std::uint32_t layer_count = 1);

// Sampler addressing modes for create_sampler. Filtering is always linear with a
// linear mip mode.
enum class SamplerAddress : std::uint8_t { repeat, clamp_to_edge, mirrored_repeat };

// Creates a linear-filtered VkSampler wrapped in an RAII Sampler.
// `max_anisotropy` > 0 enables anisotropic filtering at that level (the caller
// clamps to the device limit; pass TransferContext::max_anisotropy).
[[nodiscard]] std::expected<Sampler, core::Error>
create_sampler(VkDevice device, SamplerAddress address = SamplerAddress::repeat,
               std::uint32_t mip_levels = 1, float max_anisotropy = 0.0F);

// Debug round-trip self-test: upload bytes to a device-local buffer, copy them
// back to a host buffer, and verify they match. Returns an error on mismatch.
[[nodiscard]] std::expected<void, core::Error>
run_gpu_round_trip_self_test(const Device& device, GpuAllocator& allocator);

} // namespace engine::rhi
