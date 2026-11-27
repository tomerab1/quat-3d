#include "engine/renderer/instance_buffer.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

#include "engine/rhi/device.hpp"

namespace engine::renderer {

namespace {
// Smallest allocation, so a handful of draws never reallocates frame to frame.
constexpr std::uint32_t kMinCapacity = 256;
} // namespace

std::expected<VkDeviceAddress, core::Error>
InstanceBuffer::upload(const rhi::Device& device, rhi::GpuAllocator& allocator,
                       std::span<const InstanceData> instances) {
    if (instances.empty()) return VkDeviceAddress{0};
    const auto count = static_cast<std::uint32_t>(instances.size());

    if (count > capacity_) {
        const std::uint32_t capacity = std::max(kMinCapacity, std::bit_ceil(count));
        // Sequential-write host access lets VMA pick device-local host-visible
        // memory (ReBAR / UMA) where available; the vertex shader reads it
        // directly through the device address.
        auto buffer = allocator.create_buffer(
            static_cast<VkDeviceSize>(capacity) * sizeof(InstanceData),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!buffer) return std::unexpected(buffer.error());
        buffer_ = std::move(*buffer);
        address_ = rhi::buffer_device_address(device.handle(), buffer_);
        capacity_ = capacity;
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(count) * sizeof(InstanceData);
    std::memcpy(buffer_.mapped(), instances.data(), bytes);
    // The memory may be non-coherent; the flush is a no-op when it is coherent.
    if (auto flushed = buffer_.flush(0, bytes); !flushed) {
        return std::unexpected(flushed.error());
    }
    return address_;
}

} // namespace engine::renderer
