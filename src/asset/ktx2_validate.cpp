// KTX2 header / level-index validation (no libktx): everything libktx would
// trust when allocating or seeking is range-checked here first.

#include "engine/asset/ktx2_texture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

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

// Data format descriptor: a total size, then descriptor blocks whose sizes
// add up to it exactly. libktx walks the blocks by their declared sizes (a
// zero size loops forever), so each block must be at least a block header
// and the first one a Khronos basic block (version 1.3) holding whole
// samples.
[[nodiscard]] std::expected<void, core::Error>
validate_dfd(std::span<const std::byte> bytes, std::uint32_t offset, std::uint32_t length) {
    constexpr std::uint32_t k_block_header = 8;
    constexpr std::uint32_t k_basic_header = 24;
    constexpr std::uint32_t k_sample = 16;
    constexpr std::uint32_t k_version_1_3 = 2;
    if (length < 4 + k_basic_header || u32_at(bytes, offset) != length) {
        return fail("malformed data format descriptor");
    }
    const std::uint32_t first = offset + 4;
    if (u32_at(bytes, first) != 0 || (u32_at(bytes, first + 4) & 0xFFFFU) != k_version_1_3) {
        return fail("data format descriptor is not a Khronos basic descriptor (1.3)");
    }
    const std::uint32_t basic = u32_at(bytes, first + 4) >> 16;
    if (basic < k_basic_header + k_sample || (basic - k_basic_header) % k_sample != 0) {
        return fail("malformed basic data format descriptor block");
    }
    std::uint32_t at = first;
    std::uint32_t remaining = length - 4;
    while (remaining > 0) {
        if (remaining < k_block_header) return fail("truncated data format descriptor block");
        const std::uint32_t block = u32_at(bytes, at + 4) >> 16;
        if (block < k_block_header || block % 4 != 0 || block > remaining) {
            return fail("data format descriptor block sizes do not add up");
        }
        at += block;
        remaining -= block;
    }
    return {};
}

struct LevelRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

// BasisLZ (ETC1S) supercompression global data: a 20-byte header (endpoint /
// selector counts, then endpoints, selectors, tables and extended byte
// lengths), one 20-byte image descriptor per image (flags, RGB slice
// offset/length, alpha slice offset/length, offsets relative to the image's
// level), then the four payloads. A 2D, single-layer, single-face texture has
// one image per level.
[[nodiscard]] std::expected<void, core::Error>
validate_basis_lz(std::span<const std::byte> bytes, std::uint64_t sgd_offset,
                  std::uint64_t sgd_length, std::span<const LevelRange> levels) {
    constexpr std::uint64_t k_sgd_header = 20;
    constexpr std::uint64_t k_image_desc = 20;
    if (sgd_length < k_sgd_header) return fail("BasisLZ global data is truncated");
    const auto at = static_cast<std::size_t>(sgd_offset);
    const std::uint32_t counts = u32_at(bytes, at);
    const std::uint64_t endpoints = u32_at(bytes, at + 4);
    const std::uint64_t selectors = u32_at(bytes, at + 8);
    const std::uint64_t tables = u32_at(bytes, at + 12);
    const std::uint64_t extended = u32_at(bytes, at + 16);
    if ((counts & 0xFFFFU) == 0 || (counts >> 16) == 0 || endpoints == 0 || selectors == 0 ||
        tables == 0) {
        return fail("BasisLZ global data has no codebooks");
    }
    const std::uint64_t needed = k_sgd_header + k_image_desc * levels.size() + endpoints +
                                 selectors + tables + extended;
    if (needed > sgd_length) {
        return fail("BasisLZ global data lengths exceed its " + std::to_string(sgd_length) +
                    " bytes");
    }
    for (std::size_t level = 0; level < levels.size(); ++level) {
        const std::size_t desc = at + k_sgd_header + level * k_image_desc;
        const std::uint64_t rgb_offset = u32_at(bytes, desc + 4);
        const std::uint64_t rgb_length = u32_at(bytes, desc + 8);
        const std::uint64_t alpha_offset = u32_at(bytes, desc + 12);
        const std::uint64_t alpha_length = u32_at(bytes, desc + 16);
        const std::uint64_t level_length = levels[level].length;
        if (rgb_length == 0 || !in_file(rgb_offset, rgb_length, level_length) ||
            (alpha_length != 0 && !in_file(alpha_offset, alpha_length, level_length))) {
            return fail("BasisLZ slice of mip level " + std::to_string(level) +
                        " lies outside the level");
        }
    }
    return {};
}

} // namespace

bool is_ktx2(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < k_identifier.size()) return false;
    return std::memcmp(bytes.data(), k_identifier.data(), k_identifier.size()) == 0;
}

std::expected<Ktx2Info, core::Error> validate_ktx2(std::span<const std::byte> bytes,
                                                    const TextureLimits& limits) {
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
    if (!in_file(dfd_offset, dfd_length, size) || !in_file(kvd_offset, kvd_length, size) ||
        !in_file(sgd_offset, sgd_length, size)) {
        return fail("descriptor / metadata ranges lie outside the file");
    }
    if (auto ok = validate_dfd(bytes, dfd_offset, dfd_length); !ok) {
        return std::unexpected(ok.error());
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
    std::vector<LevelRange> ranges(levels);
    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::size_t at = k_header_bytes + static_cast<std::size_t>(level) * k_level_entry_bytes;
        const std::uint64_t offset = u64_at(bytes, at);
        const std::uint64_t length = u64_at(bytes, at + 8);
        const std::uint64_t inflated = u64_at(bytes, at + 16);
        if (length == 0 || !in_file(offset, length, size)) {
            return fail("mip level " + std::to_string(level) + " lies outside the file");
        }
        ranges[level] = {offset, length};
        if (info.payload == Ktx2Payload::etc1s) { // sizes live in the global data
            info.stored_bytes += length;
            continue;
        }
        const std::uint32_t lw = std::max(width >> level, 1U);
        const std::uint32_t lh = std::max(height >> level, 1U);
        const std::uint64_t expected =
            texel_level_size(info.raw_format.value_or(TexelFormat::astc_4x4), lw, lh);
        if (inflated != expected || (scheme == k_scheme_none && length != expected)) {
            return fail("mip level " + std::to_string(level) + " size does not match its extent");
        }
        inflated_total += inflated;
    }
    if (info.payload != Ktx2Payload::etc1s) info.stored_bytes = inflated_total;
    // KTX2 stores the smallest level first: each level ends at or before the
    // start of the next larger one. libktx sizes its data buffer from level 0's
    // end minus the smallest level's start, so overlapping or reordered
    // levels would make that size wrap.
    for (std::uint32_t level = 1; level < levels; ++level) {
        if (ranges[level].offset + ranges[level].length > ranges[level - 1].offset) {
            return fail("mip levels " + std::to_string(level - 1) + " and " +
                        std::to_string(level) + " overlap or are out of order");
        }
    }
    if (inflated_total > limits.max_decoded_bytes) {
        return fail("decoded size exceeds the configured limit");
    }
    if (info.payload == Ktx2Payload::etc1s) {
        if (auto ok = validate_basis_lz(bytes, sgd_offset, sgd_length, ranges); !ok) {
            return std::unexpected(ok.error());
        }
    }
    return info;
}

} // namespace engine::asset
