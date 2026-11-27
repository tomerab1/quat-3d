#pragma once

// Guarded Radiance RGBE (.hdr) decoding of untrusted bytes (R21: HDRI
// environments). Like decode_png_jpeg, the header is parsed and checked
// against TextureLimits and the load's DecodeBudget before stb_image
// allocates anything: only the "-Y <h> +X <w>" orientation and the
// 32-bit_rle_rgbe format are accepted (what stb_image reads), and the
// decoded float buffer is adopted without a copy.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

#include "engine/asset/texture_data.hpp"
#include "engine/asset/texture_limits.hpp"
#include "engine/core/error.hpp"

namespace engine::asset {

// A decoded high-dynamic-range image: linear RGB, three floats per texel,
// rows top to bottom (equirectangular panoramas put the zenith on row 0).
struct HdrImage {
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    PixelBuffer   pixels; // width * height * 3 floats

    [[nodiscard]] std::span<const float> rgb() const noexcept {
        return {reinterpret_cast<const float*>(pixels.data()),
                static_cast<std::size_t>(width) * height * 3};
    }
};

// Bytes of an HDR decode of `width` x `height` (RGB32F).
[[nodiscard]] constexpr std::uint64_t hdr_decoded_bytes(std::uint32_t width,
                                                        std::uint32_t height) noexcept {
    return static_cast<std::uint64_t>(width) * height * 3 * sizeof(float);
}

// True when `bytes` start with the Radiance signature ("#?RADIANCE" / "#?RGBE").
[[nodiscard]] bool is_radiance_hdr(std::span<const std::byte> bytes) noexcept;

[[nodiscard]] std::expected<HdrImage, core::Error>
decode_radiance_hdr(std::span<const std::byte> encoded, const TextureLimits& limits = {},
                    DecodeBudget* budget = nullptr);

// Reads `path` (at most limits.max_file_bytes) and decodes it.
[[nodiscard]] std::expected<HdrImage, core::Error>
load_radiance_hdr(const std::filesystem::path& path, const TextureLimits& limits = {});

} // namespace engine::asset
