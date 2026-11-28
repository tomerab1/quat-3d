#pragma once

// NonFiniteCounters — debug counters for NaN / Inf values that reached the
// guards in the post-processing passes (issue #45).
//
// The guards (bloom's prefilter, TAA's history) replace a non-finite value so
// it cannot spread or persist; in debug and validation builds they also count
// each replacement into a small host-visible buffer through a buffer device
// address passed in the pass's push constants (0 = counting off, the release
// default). The frame loop reads a slot's counters after waiting on its fence
// and logs once per frame when any is non-zero, so a NaN source is reported
// instead of silently hidden.

#include <array>
#include <cstdint>
#include <expected>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::rhi {
class Device;
}

namespace engine::renderer {

enum class NonFiniteSource : std::uint32_t {
    bloom_prefilter, // non-finite HDR texels dropped by the first bloom downsample
    taa_history,     // non-finite reprojection / history / current samples in TAA
    count,
};

[[nodiscard]] const char* non_finite_source_name(NonFiniteSource source);

class NonFiniteCounters {
public:
    static constexpr std::uint32_t k_sources = static_cast<std::uint32_t>(NonFiniteSource::count);
    using Counts = std::array<std::uint32_t, k_sources>;

    // One block of counters per frame slot.
    [[nodiscard]] static std::expected<NonFiniteCounters, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, std::uint32_t frame_slots);

    NonFiniteCounters() = default;

    // Device address of `source`'s counter for `slot` (a uint32 the shader
    // atomically increments); 0 when not created.
    [[nodiscard]] VkDeviceAddress address(std::uint32_t slot, NonFiniteSource source) const;

    // Reads and zeroes `slot`'s counters. Call after the slot's fence (its
    // previous frame's GPU work) has completed and before recording into it.
    [[nodiscard]] Counts collect(std::uint32_t slot);

    [[nodiscard]] bool valid() const { return address_ != 0; }

private:
    rhi::GpuBuffer  buffer_;
    VkDeviceAddress address_ = 0;
    std::uint32_t   slots_ = 0;
};

} // namespace engine::renderer
