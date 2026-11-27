// NonFiniteCounters — see engine/renderer/non_finite_counters.hpp.

#include "engine/renderer/non_finite_counters.hpp"

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
    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(frame_slots) * k_sources * sizeof(std::uint32_t);
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
    out.slots_ = frame_slots;
    return out;
}

VkDeviceAddress NonFiniteCounters::address(std::uint32_t slot, NonFiniteSource source) const {
    if (address_ == 0 || slot >= slots_ || source == NonFiniteSource::count) return 0;
    const auto index = static_cast<VkDeviceAddress>(slot) * k_sources +
                       static_cast<std::uint32_t>(source);
    return address_ + index * sizeof(std::uint32_t);
}

NonFiniteCounters::Counts NonFiniteCounters::collect(std::uint32_t slot) {
    Counts counts{};
    if (address_ == 0 || slot >= slots_) return counts;
    if (auto inv = buffer_.invalidate_mapped(); !inv) return counts;
    auto* base = static_cast<std::uint32_t*>(buffer_.mapped()) +
                 static_cast<std::size_t>(slot) * k_sources;
    for (std::uint32_t i = 0; i < k_sources; ++i) {
        counts[i] = base[i];
        base[i] = 0;
    }
    (void)buffer_.flush(static_cast<VkDeviceSize>(slot) * k_sources * sizeof(std::uint32_t),
                        k_sources * sizeof(std::uint32_t));
    return counts;
}

} // namespace engine::renderer
