#include "engine/renderer/instance_buffer.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

#include "engine/rhi/device.hpp"

namespace engine::renderer {

namespace {
// Smallest allocation, so a handful of draws never reallocates frame to frame.
constexpr VkDeviceSize kMinBytes = 256 * sizeof(InstanceData);
} // namespace

std::expected<VkDeviceAddress, core::Error>
InstanceBuffer::upload(const rhi::Device& device, rhi::GpuAllocator& allocator,
                       std::span<const InstanceData> instances) {
    return upload_bytes(device, allocator, instances.data(), instances.size_bytes());
}

std::expected<VkDeviceAddress, core::Error>
InstanceBuffer::upload(const rhi::Device& device, rhi::GpuAllocator& allocator,
                       std::span<const InstanceMotion> motion) {
    return upload_bytes(device, allocator, motion.data(), motion.size_bytes());
}

std::expected<VkDeviceAddress, core::Error>
InstanceBuffer::upload_bytes(const rhi::Device& device, rhi::GpuAllocator& allocator,
                             const void* data, VkDeviceSize bytes) {
    if (bytes == 0) return VkDeviceAddress{0};

    if (bytes > capacity_) {
        const VkDeviceSize capacity = std::max(kMinBytes, std::bit_ceil(bytes));
        // Sequential-write host access lets VMA pick device-local host-visible
        // memory (ReBAR / UMA) where available; the vertex shader reads it
        // directly through the device address.
        auto buffer = allocator.create_buffer(
            capacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!buffer) return std::unexpected(buffer.error());
        buffer_ = std::move(*buffer);
        address_ = rhi::buffer_device_address(device.handle(), buffer_);
        capacity_ = capacity;
    }

    std::memcpy(buffer_.mapped(), data, bytes);
    // The memory may be non-coherent; the flush is a no-op when it is coherent.
    if (auto flushed = buffer_.flush(0, bytes); !flushed) {
        return std::unexpected(flushed.error());
    }
    return address_;
}

} // namespace engine::renderer
