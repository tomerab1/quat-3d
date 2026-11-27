#pragma once

// Limits applied to every untrusted texture before anything is allocated: one
// configuration shared by the PNG/JPEG decoder, the KTX2 loader and the
// cooker, plus a running budget for one load that decodes many images in
// parallel.

#include <atomic>
#include <cstdint>

#include "engine/rhi/texture_caps.hpp"

namespace engine::asset {

struct TextureLimits {
    std::uint32_t max_dimension           = 8192;          // per side, any format
    std::uint64_t max_decoded_bytes       = 512ULL << 20;  // one image, all levels
    std::uint64_t max_file_bytes          = 512ULL << 20;  // one encoded image
    std::uint64_t max_total_decoded_bytes = 2ULL << 30;    // all images of one load
};

// `limits` with max_dimension lowered to the device's maxImageDimension2D
// when that is known and smaller.
[[nodiscard]] TextureLimits clamp_to_device(TextureLimits limits,
                                            const rhi::TextureCaps& caps) noexcept;

// Decoded bytes still available to one load. Thread-safe: concurrent decodes
// reserve their output size before allocating it.
class DecodeBudget {
public:
    explicit DecodeBudget(std::uint64_t total) noexcept : total_(total) {}
    DecodeBudget(const DecodeBudget&) = delete;
    DecodeBudget& operator=(const DecodeBudget&) = delete;

    // Claims `bytes`; false (claiming nothing) when that would exceed the total.
    [[nodiscard]] bool reserve(std::uint64_t bytes) noexcept;
    [[nodiscard]] std::uint64_t used() const noexcept { return used_.load(); }
    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }

private:
    std::atomic<std::uint64_t> used_{0};
    std::uint64_t              total_;
};

} // namespace engine::asset
