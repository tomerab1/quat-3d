#pragma once

// Screenshot encoding for render.screenshot (ROADMAP A3): sRGB-correct
// downscaling, PNG encoding and base64 — CPU only, so the frame loop can run
// it off the main thread on pixels copied out of a GPU readback buffer.

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "engine/core/error.hpp"

namespace engine::remote {

struct EncodedImage {
    std::uint32_t             width = 0;
    std::uint32_t             height = 0;
    std::vector<std::uint8_t> png;
};

// Tightly packed 8-bit RGB (3 bytes per pixel).
struct RgbImage {
    std::uint32_t             width = 0;
    std::uint32_t             height = 0;
    std::vector<std::uint8_t> pixels;
};

// Converts 4-byte texels (RGBA, or BGRA when `bgra`; alpha ignored) of
// sRGB-encoded colour to RGB, area-averaging in linear light so the longer
// side is at most `max_size` (never upscales). `row_pitch` is the byte stride
// between rows (>= width * 4).
[[nodiscard]] std::expected<RgbImage, core::Error>
downscale_srgb(std::span<const std::uint8_t> texels, std::uint32_t width, std::uint32_t height,
               std::size_t row_pitch, bool bgra, std::uint32_t max_size);

[[nodiscard]] std::expected<std::vector<std::uint8_t>, core::Error> encode_png(const RgbImage& image);

// downscale_srgb + encode_png.
[[nodiscard]] std::expected<EncodedImage, core::Error>
encode_screenshot(std::span<const std::uint8_t> texels, std::uint32_t width, std::uint32_t height,
                  std::size_t row_pitch, bool bgra, std::uint32_t max_size);

// Standard base64 (RFC 4648, with padding).
[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> bytes);

} // namespace engine::remote
