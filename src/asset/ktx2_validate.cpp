// KTX2 header / level-index validation (no libktx): everything libktx would
// trust when allocating or seeking is range-checked here first.

#include "engine/asset/ktx2_texture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <string>

namespace engine::asset {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"KTX2: " + std::move(what)});
}

constexpr std::array<std::uint8_t, 12> k_identifier{0xAB, 'K', 'T', 'X', ' ', '2',
                                                    '0', 0xBB, '\r', '\n', 0x1A, '\n'};
constexpr std::size_t k_header_bytes = 80;
constexpr std::size_t k_level_entry_bytes = 24;

// VkFormat values (vulkan_core.h) a raw payload may hold.
constexpr std::uint32_t k_vk_rgba8_unorm = 37;
constexpr std::uint32_t k_vk_rgba8_srgb = 43;
constexpr std::uint32_t k_vk_bc4_unorm = 139;
constexpr std::uint32_t k_vk_bc5_unorm = 141;
constexpr std::uint32_t k_vk_bc7_unorm = 145;
constexpr std::uint32_t k_vk_bc7_srgb = 146;
constexpr std::uint32_t k_vk_astc4_unorm = 157;
constexpr std::uint32_t k_vk_astc4_srgb = 158;

// Supercompression schemes.
constexpr std::uint32_t k_scheme_none = 0;
constexpr std::uint32_t k_scheme_basis_lz = 1;
constexpr std::uint32_t k_scheme_zstd = 2;

// Khronos data format descriptor colour models.
constexpr std::uint8_t k_model_etc1s = 163;
constexpr std::uint8_t k_model_uastc = 166;

[[nodiscard]] std::uint32_t u32_at(std::span<const std::byte> b, std::size_t at) noexcept {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[at + i])) << (8 * i);
    }
    return v;
}

[[nodiscard]] std::uint64_t u64_at(std::span<const std::byte> b, std::size_t at) noexcept {
    return static_cast<std::uint64_t>(u32_at(b, at)) |
           (static_cast<std::uint64_t>(u32_at(b, at + 4)) << 32);
}

// [offset, offset + length) lies inside a file of `size` bytes.
[[nodiscard]] bool in_file(std::uint64_t offset, std::uint64_t length, std::size_t size) noexcept {
    return offset <= size && length <= size - offset;
}

struct RawFormat {
    TexelFormat format;
    bool        srgb;
};

[[nodiscard]] std::optional<RawFormat> raw_format(std::uint32_t vk_format) noexcept {
    switch (vk_format) {
        case k_vk_rgba8_unorm: return RawFormat{TexelFormat::rgba8, false};
        case k_vk_rgba8_srgb:  return RawFormat{TexelFormat::rgba8, true};
        case k_vk_bc4_unorm:   return RawFormat{TexelFormat::bc4, false};
        case k_vk_bc5_unorm:   return RawFormat{TexelFormat::bc5, false};
        case k_vk_bc7_unorm:   return RawFormat{TexelFormat::bc7, false};
        case k_vk_bc7_srgb:    return RawFormat{TexelFormat::bc7, true};
        case k_vk_astc4_unorm: return RawFormat{TexelFormat::astc_4x4, false};
        case k_vk_astc4_srgb:  return RawFormat{TexelFormat::astc_4x4, true};
        default:               return std::nullopt;
    }
}

[[nodiscard]] std::uint32_t mip_count(std::uint32_t w, std::uint32_t h) noexcept {
    return static_cast<std::uint32_t>(std::bit_width(std::max(w, h)));
}

} // namespace

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

bool is_ktx2(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < k_identifier.size()) return false;
    return std::memcmp(bytes.data(), k_identifier.data(), k_identifier.size()) == 0;
}

std::expected<Ktx2Info, core::Error> validate_ktx2(std::span<const std::byte> bytes,
                                                    const Ktx2Limits& limits) {
    const std::size_t size = bytes.size();
    if (size > limits.max_file_bytes) return fail("file larger than the configured limit");
    if (size < k_header_bytes || !is_ktx2(bytes)) return fail("not a KTX2 file");

    const std::uint32_t vk_format = u32_at(bytes, 12);
    const std::uint32_t width = u32_at(bytes, 20);
    const std::uint32_t height = u32_at(bytes, 24);
    const std::uint32_t depth = u32_at(bytes, 28);
    const std::uint32_t layers = u32_at(bytes, 32);
    const std::uint32_t faces = u32_at(bytes, 36);
    const std::uint32_t levels = u32_at(bytes, 40);
    const std::uint32_t scheme = u32_at(bytes, 44);
    const std::uint32_t dfd_offset = u32_at(bytes, 48);
    const std::uint32_t dfd_length = u32_at(bytes, 52);
    const std::uint32_t kvd_offset = u32_at(bytes, 56);
    const std::uint32_t kvd_length = u32_at(bytes, 60);
    const std::uint64_t sgd_offset = u64_at(bytes, 64);
    const std::uint64_t sgd_length = u64_at(bytes, 72);

    if (width == 0 || height == 0 || depth != 0) {
        return fail("only 2D textures are supported (1D / 3D rejected)");
    }
    if (width > limits.max_dimension || height > limits.max_dimension) {
        return fail("dimensions " + std::to_string(width) + "x" + std::to_string(height) +
                    " exceed the limit of " + std::to_string(limits.max_dimension));
    }
    if (layers > 1 || faces != 1) return fail("array and cubemap textures are not supported");
    if (levels == 0) return fail("no mip levels stored (runtime mip generation unsupported)");
    if (levels > mip_count(width, height)) return fail("more mip levels than the extent allows");
    if (k_header_bytes + static_cast<std::uint64_t>(levels) * k_level_entry_bytes > size) {
        return fail("truncated level index");
    }
    if (!in_file(dfd_offset, dfd_length, size) || dfd_length < 16 ||
        !in_file(kvd_offset, kvd_length, size) || !in_file(sgd_offset, sgd_length, size)) {
        return fail("descriptor / metadata ranges lie outside the file");
    }

    Ktx2Info info;
    info.width = width;
    info.height = height;
    info.levels = levels;
    if (vk_format == 0) {
        const auto model = std::to_integer<std::uint8_t>(bytes[dfd_offset + 12]);
        if (scheme == k_scheme_basis_lz && model == k_model_etc1s) {
            if (sgd_length == 0) return fail("BasisLZ payload without global data");
            info.payload = Ktx2Payload::etc1s;
        } else if ((scheme == k_scheme_none || scheme == k_scheme_zstd) &&
                   model == k_model_uastc) {
            info.payload = Ktx2Payload::uastc;
        } else {
            return fail("unsupported payload (expected ETC1S/BasisLZ or UASTC)");
        }
    } else {
        const std::optional<RawFormat> raw = raw_format(vk_format);
        if (!raw) return fail("unsupported VkFormat " + std::to_string(vk_format));
        if (scheme != k_scheme_none && scheme != k_scheme_zstd) {
            return fail("unsupported supercompression scheme for a raw format");
        }
        info.payload = Ktx2Payload::raw;
        info.raw_format = raw->format;
        info.raw_srgb = raw->srgb;
    }

    std::uint64_t inflated_total = 0;
    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::size_t at = k_header_bytes + static_cast<std::size_t>(level) * k_level_entry_bytes;
        const std::uint64_t offset = u64_at(bytes, at);
        const std::uint64_t length = u64_at(bytes, at + 8);
        const std::uint64_t inflated = u64_at(bytes, at + 16);
        if (length == 0 || !in_file(offset, length, size)) {
            return fail("mip level " + std::to_string(level) + " lies outside the file");
        }
        if (info.payload == Ktx2Payload::etc1s) continue; // sizes live in the global data
        const std::uint32_t lw = std::max(width >> level, 1U);
        const std::uint32_t lh = std::max(height >> level, 1U);
        const std::uint64_t expected =
            texel_level_size(info.raw_format.value_or(TexelFormat::astc_4x4), lw, lh);
        if (inflated != expected || (scheme == k_scheme_none && length != expected)) {
            return fail("mip level " + std::to_string(level) + " size does not match its extent");
        }
        inflated_total += inflated;
    }
    if (inflated_total > limits.max_decoded_bytes) {
        return fail("decoded size exceeds the configured limit");
    }
    return info;
}

} // namespace engine::asset
