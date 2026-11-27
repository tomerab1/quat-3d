// Guarded PNG / JPEG decode through stb_image (see image_decode.hpp).

#include "engine/asset/image_decode.hpp"

#include <array>
#include <climits>
#include <cstring>
#include <optional>
#include <string>

// Declarations only: the implementation (PNG + JPEG only, dimension-capped)
// is compiled once in stb_image_impl.cpp.
#include <stb_image.h>

namespace engine::asset {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"image decode: " + std::move(what)});
}

constexpr std::array<std::uint8_t, 8> k_png_signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

void stb_free(void* p) { stbi_image_free(p); }

struct Extent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

[[nodiscard]] std::uint32_t be(std::span<const std::byte> b, std::size_t at, std::size_t n) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v = (v << 8) | std::to_integer<std::uint32_t>(b[at + i]);
    return v;
}

// The extent a PNG's IHDR or a JPEG's SOFn marker declares, read without
// stb_image so the limits can be reported precisely (stb refuses anything
// past STBI_MAX_DIMENSIONS with a generic failure).
[[nodiscard]] std::optional<Extent> declared_extent(std::span<const std::byte> b) {
    if (b.size() >= k_png_signature.size() &&
        std::memcmp(b.data(), k_png_signature.data(), k_png_signature.size()) == 0) {
        if (b.size() < 24 || std::memcmp(b.data() + 12, "IHDR", 4) != 0) return std::nullopt;
        return Extent{be(b, 16, 4), be(b, 20, 4)};
    }
    std::size_t i = 2; // after SOI
    while (i + 4 <= b.size()) {
        if (b[i] != std::byte{0xFF}) return std::nullopt;
        const auto marker = std::to_integer<std::uint32_t>(b[i + 1]);
        if (marker == 0xFF) { // fill byte
            ++i;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) { // no length
            i += 2;
            continue;
        }
        const std::uint32_t length = be(b, i + 2, 2);
        if (length < 2) return std::nullopt;
        const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
                         marker != 0xCC;
        if (sof) {
            if (i + 9 > b.size()) return std::nullopt;
            return Extent{be(b, i + 7, 2), be(b, i + 5, 2)};
        }
        i += 2 + length;
    }
    return std::nullopt;
}

} // namespace

bool is_png_or_jpeg(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() >= k_png_signature.size() &&
        std::memcmp(bytes.data(), k_png_signature.data(), k_png_signature.size()) == 0) {
        return true;
    }
    return bytes.size() >= 3 && bytes[0] == std::byte{0xFF} && bytes[1] == std::byte{0xD8} &&
           bytes[2] == std::byte{0xFF};
}

std::expected<TextureData, core::Error>
decode_png_jpeg(std::span<const std::byte> encoded, TextureColorSpace color_space,
                const TextureLimits& limits, DecodeBudget* budget) {
    if (encoded.size() > limits.max_file_bytes || encoded.size() > static_cast<std::size_t>(INT_MAX)) {
        return fail("encoded image of " + std::to_string(encoded.size()) +
                    " bytes exceeds the file size limit");
    }
    if (!is_png_or_jpeg(encoded)) return fail("unsupported image format (PNG and JPEG only)");
    const std::optional<Extent> extent = declared_extent(encoded);
    if (!extent || extent->width == 0 || extent->height == 0) {
        return fail("corrupt PNG/JPEG header (no image size)");
    }
    const std::uint32_t uw = extent->width;
    const std::uint32_t uh = extent->height;
    if (uw > limits.max_dimension || uh > limits.max_dimension) {
        return fail("image is " + std::to_string(uw) + "x" + std::to_string(uh) +
                    ", larger than the limit of " + std::to_string(limits.max_dimension) +
                    " per side");
    }
    const std::uint64_t decoded = texel_level_size(TexelFormat::rgba8, uw, uh);
    if (decoded > limits.max_decoded_bytes) {
        return fail("image would decode to " + std::to_string(decoded) +
                    " bytes, over the per-image limit of " +
                    std::to_string(limits.max_decoded_bytes));
    }
    // stb_image's own header parse must agree before it allocates anything.
    const auto* bytes = reinterpret_cast<const stbi_uc*>(encoded.data());
    const int len = static_cast<int>(encoded.size());
    int w = 0;
    int h = 0;
    int channels = 0;
    if (stbi_info_from_memory(bytes, len, &w, &h, &channels) == 0 ||
        static_cast<std::uint32_t>(w) != uw || static_cast<std::uint32_t>(h) != uh) {
        return fail("corrupt or unsupported PNG/JPEG header");
    }
    if (budget != nullptr && !budget->reserve(decoded)) {
        return fail("image would exceed the load's decoded-texture budget of " +
                    std::to_string(budget->total()) + " bytes");
    }

    int dw = 0;
    int dh = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes, len, &dw, &dh, &channels, STBI_rgb_alpha);
    PixelBuffer owned = PixelBuffer::adopt(pixels, static_cast<std::size_t>(decoded), &stb_free);
    if (pixels == nullptr) return fail("PNG/JPEG data is corrupt");
    if (dw != w || dh != h) return fail("decoded size differs from the header");

    TextureData out;
    out.pixels = std::move(owned);
    out.width = uw;
    out.height = uh;
    out.color_space = color_space;
    return out;
}

} // namespace engine::asset
