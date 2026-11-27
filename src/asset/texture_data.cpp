// PixelBuffer, texture limits and the per-load decode budget.

#include <algorithm>
#include <cstdlib>

#include "engine/asset/texture_data.hpp"
#include "engine/asset/texture_limits.hpp"

namespace engine::asset {

PixelBuffer PixelBuffer::allocate(std::size_t size) {
    PixelBuffer out;
    if (size == 0) return out;
    void* p = std::malloc(size); // NOLINT(cppcoreguidelines-no-malloc): owned by data_
    if (p == nullptr) return out;
    return adopt(p, size, &std::free);
}

PixelBuffer PixelBuffer::adopt(void* data, std::size_t size, FreeFn free_fn) noexcept {
    PixelBuffer out;
    out.data_ = std::unique_ptr<std::byte, PixelRelease>(static_cast<std::byte*>(data),
                                                         PixelRelease{free_fn});
    out.size_ = data != nullptr ? size : 0;
    return out;
}

std::uint64_t texel_level_size(TexelFormat format, std::uint32_t width,
                               std::uint32_t height) noexcept {
    const std::uint64_t bw = (static_cast<std::uint64_t>(width) + 3) / 4;
    const std::uint64_t bh = (static_cast<std::uint64_t>(height) + 3) / 4;
    switch (format) {
        case TexelFormat::rgba8:    return static_cast<std::uint64_t>(width) * height * 4;
        case TexelFormat::bc4:      return bw * bh * 8;
        case TexelFormat::bc5:
        case TexelFormat::bc7:
        case TexelFormat::astc_4x4: return bw * bh * 16;
    }
    return 0;
}

TextureLimits clamp_to_device(TextureLimits limits, const rhi::TextureCaps& caps) noexcept {
    if (caps.max_dimension_2d != 0) {
        limits.max_dimension = std::min(limits.max_dimension, caps.max_dimension_2d);
    }
    return limits;
}

bool DecodeBudget::reserve(std::uint64_t bytes) noexcept {
    std::uint64_t used = used_.load(std::memory_order_relaxed);
    do {
        if (bytes > total_ || used > total_ - bytes) return false;
    } while (!used_.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    return true;
}

void DecodeBudget::release(std::uint64_t bytes) noexcept {
    std::uint64_t used = used_.load(std::memory_order_relaxed);
    while (!used_.compare_exchange_weak(used, used - std::min(used, bytes),
                                        std::memory_order_relaxed)) {
    }
}

} // namespace engine::asset
