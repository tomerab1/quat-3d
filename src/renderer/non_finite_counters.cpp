// NonFiniteCounters — see engine/renderer/non_finite_counters.hpp.

#include "engine/renderer/non_finite_counters.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

#include "engine/rhi/device.hpp"

namespace engine::renderer {

const char* non_finite_source_name(NonFiniteSource source) {
    switch (source) {
    case NonFiniteSource::bloom_prefilter: return "bloom prefilter";
    case NonFiniteSource::taa_history: return "taa history";
    case NonFiniteSource::count: break;
    }
    return "?";
}

std::expected<NonFiniteCounters, core::Error>
NonFiniteCounters::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                          std::uint32_t frame_slots) {
    if (frame_slots == 0) return std::unexpected(core::Error{"non-finite counters: no slots"});
    // Each counter starts its own storage-buffer descriptor, so counters sit
    // minStorageBufferOffsetAlignment apart (a power of two, at most 256).
    const VkDeviceSize align =
        std::max<VkDeviceSize>(device.properties().limits.minStorageBufferOffsetAlignment, 1);
    const VkDeviceSize stride = (k_counter_range + align - 1) / align * align;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(frame_slots) * k_sources * stride;
    auto buffer = allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!buffer) return std::unexpected(buffer.error());
    NonFiniteCounters out;
    out.buffer_ = std::move(*buffer);
    std::memset(out.buffer_.mapped(), 0, static_cast<std::size_t>(bytes));
    if (auto f = out.buffer_.flush(); !f) return std::unexpected(f.error());
    out.address_ = rhi::buffer_device_address(device.handle(), out.buffer_);
    out.stride_ = stride;
    out.slots_ = frame_slots;
    return out;
}

VkDeviceAddress NonFiniteCounters::address(std::uint32_t slot, NonFiniteSource source) const {
    if (address_ == 0 || slot >= slots_ || source == NonFiniteSource::count) return 0;
    const auto index = static_cast<VkDeviceAddress>(slot) * k_sources +
                       static_cast<std::uint32_t>(source);
    return address_ + index * stride_;
}

void NonFiniteCounters::make_host_visible(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

NonFiniteCounters::Counts NonFiniteCounters::collect(std::uint32_t slot) {
    Counts counts{};
    if (address_ == 0 || slot >= slots_) return counts;
    if (auto inv = buffer_.invalidate_mapped(); !inv) return counts;
    const VkDeviceSize first = static_cast<VkDeviceSize>(slot) * k_sources * stride_;
    auto* base = static_cast<std::byte*>(buffer_.mapped()) + first;
    for (std::uint32_t i = 0; i < k_sources; ++i) {
        std::byte* counter = base + static_cast<std::size_t>(i * stride_);
        std::memcpy(&counts[i], counter, sizeof(std::uint32_t));
        std::memset(counter, 0, sizeof(std::uint32_t));
    }
    (void)buffer_.flush(first, k_sources * stride_);
    return counts;
}

} // namespace engine::renderer
