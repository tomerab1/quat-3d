#pragma once

// Guarded PNG / JPEG decoding (stb_image) of untrusted bytes, shared by the
// glTF loader and the cooker. The header is inspected first: other formats,
// images over `limits` (dimensions, decoded bytes) and images the load's
// budget cannot hold are rejected before any pixel memory is allocated, and
// the decoder's buffer is adopted as-is (no second copy).

#include <cstddef>
#include <expected>
#include <span>

#include "engine/asset/texture_data.hpp"
#include "engine/asset/texture_limits.hpp"
#include "engine/core/error.hpp"

namespace engine::asset {

// True when `bytes` start with a PNG or JPEG signature.
[[nodiscard]] bool is_png_or_jpeg(std::span<const std::byte> bytes) noexcept;

// Decodes to tightly packed RGBA8 (levels empty: mips are generated on upload).
[[nodiscard]] std::expected<TextureData, core::Error>
decode_png_jpeg(std::span<const std::byte> encoded, TextureColorSpace color_space,
                const TextureLimits& limits = {}, DecodeBudget* budget = nullptr);

} // namespace engine::asset
