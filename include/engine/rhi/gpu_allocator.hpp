#pragma once

#include <cstdint>
#include <expected>

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

    [[nodiscard]] std::expected<GpuImage, core::Error>
    create_image(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                 std::uint32_t mip_levels = 1);

private:
    VmaAllocator allocator_ = nullptr;
};

// Immutable transfer context (transfer-capable queue + a command pool created on
// its family) used by the staging upload helper.
struct TransferContext {
    VkDevice      device = VK_NULL_HANDLE;
    VkCommandPool pool   = VK_NULL_HANDLE;
    VkQueue       queue  = VK_NULL_HANDLE;
};

// Creates a device-local buffer and uploads `data` into it through a temporary
// staging buffer (blocking copy via the transfer context). The returned buffer
// always has TRANSFER_DST added to `usage`.
[[nodiscard]] std::expected<GpuBuffer, core::Error>
upload_device_buffer(GpuAllocator& allocator, const TransferContext& transfer,
                     const void* data, VkDeviceSize size, VkBufferUsageFlags usage);

// Debug round-trip self-test: upload bytes to a device-local buffer, copy them
// back to a host buffer, and verify they match. Returns an error on mismatch.
[[nodiscard]] std::expected<void, core::Error>
run_gpu_round_trip_self_test(const Device& device, GpuAllocator& allocator);

} // namespace engine::rhi
