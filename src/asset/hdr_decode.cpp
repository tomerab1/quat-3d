// Guarded Radiance RGBE decode through stb_image (see hdr_decode.hpp).

#include "engine/asset/hdr_decode.hpp"

#include <array>
#include <climits>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/utf8_path.hpp"

// Declarations only: the implementation (PNG, JPEG and HDR, dimension-capped)
// is compiled once in stb_image_impl.cpp.
#include <stb_image.h>

namespace engine::asset {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{"hdr decode: " + std::move(what)});
}

constexpr std::string_view k_radiance = "#?RADIANCE\n";
constexpr std::string_view k_rgbe = "#?RGBE\n";
// stb_image reads header lines into a buffer of this many bytes (longer lines
// are truncated); the header itself is bounded so a file of newlines cannot
// make the parse walk the whole input.
constexpr std::size_t k_max_header_bytes = 64 * 1024;

void stb_free(void* p) { stbi_image_free(p); }

struct Extent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t   data_offset = 0; // first byte after the resolution line
};

// The fewest pixel-data bytes a valid file of `e` can hold: run-length
// encoded scanlines (widths 8..32767) need a 4-byte marker plus at least one
// 2-byte run per 128 texels for each of the four components; flat ones 4
// bytes per texel. stb_image reads past the end as zeros instead of failing,
// so a truncated file is refused here.
[[nodiscard]] std::uint64_t min_data_bytes(const Extent& e) {
    const std::uint64_t w = e.width;
    const std::uint64_t per_line = (w >= 8 && w < 32768) ? 4 + 4 * 2 * ((w + 127) / 128) : 4 * w;
    return per_line * e.height;
}

[[nodiscard]] bool starts_with(std::span<const std::byte> b, std::string_view s) {
    return b.size() >= s.size() && std::memcmp(b.data(), s.data(), s.size()) == 0;
}

// A run of decimal digits as a positive int (no sign, no overflow).
[[nodiscard]] std::optional<std::uint32_t> parse_dimension(std::string_view& s) {
    std::uint64_t v = 0;
    std::size_t n = 0;
    while (n < s.size() && s[n] >= '0' && s[n] <= '9') {
        v = v * 10 + static_cast<std::uint64_t>(s[n] - '0');
        if (v > INT_MAX) return std::nullopt;
        ++n;
    }
    if (n == 0) return std::nullopt;
    s.remove_prefix(n);
    return static_cast<std::uint32_t>(v);
}

// The header as stb_image reads it: signature, variable lines up to a blank
// line (FORMAT must be 32-bit_rle_rgbe), then "-Y <height> +X <width>".
[[nodiscard]] std::expected<Extent, core::Error> parse_header(std::span<const std::byte> b) {
    const auto* chars = reinterpret_cast<const char*>(b.data());
    const std::string_view all(chars, std::min(b.size(), k_max_header_bytes));
    std::size_t at = starts_with(b, k_radiance) ? k_radiance.size() : k_rgbe.size();
    bool format_ok = false;
    for (;;) {
        const std::size_t end = all.find('\n', at);
        if (end == std::string_view::npos) return fail("header has no end (blank line)");
        const std::string_view line = all.substr(at, end - at);
        at = end + 1;
        if (line.empty()) break;
        if (line.starts_with("FORMAT=")) {
            if (line != "FORMAT=32-bit_rle_rgbe") {
                return fail("unsupported pixel format '" + std::string(line.substr(7)) +
                            "' (32-bit_rle_rgbe only)");
            }
            format_ok = true;
        }
    }
    if (!format_ok) return fail("header has no FORMAT=32-bit_rle_rgbe line");
    const std::size_t end = all.find('\n', at);
    if (end == std::string_view::npos) return fail("missing resolution line");
    std::string_view res = all.substr(at, end - at);
    if (!res.starts_with("-Y ")) return fail("unsupported orientation (only -Y <h> +X <w>)");
    res.remove_prefix(3);
    const std::optional<std::uint32_t> h = parse_dimension(res);
    while (!res.empty() && res.front() == ' ') res.remove_prefix(1);
    if (!h || !res.starts_with("+X ")) return fail("unsupported orientation (only -Y <h> +X <w>)");
    res.remove_prefix(3);
    const std::optional<std::uint32_t> w = parse_dimension(res);
    if (!w || *w == 0 || *h == 0) return fail("corrupt resolution line");
    return Extent{*w, *h, end + 1};
}

} // namespace

bool is_radiance_hdr(std::span<const std::byte> bytes) noexcept {
    return starts_with(bytes, k_radiance) || starts_with(bytes, k_rgbe);
}

std::expected<HdrImage, core::Error> decode_radiance_hdr(std::span<const std::byte> encoded,
                                                         const TextureLimits& limits,
                                                         DecodeBudget* budget) {
    if (encoded.size() > limits.max_file_bytes ||
        encoded.size() > static_cast<std::size_t>(INT_MAX)) {
        return fail("encoded image of " + std::to_string(encoded.size()) +
                    " bytes exceeds the file size limit");
    }
    if (!is_radiance_hdr(encoded)) return fail("not a Radiance .hdr file (#?RADIANCE)");
    auto extent = parse_header(encoded);
    if (!extent) return std::unexpected(extent.error());
    const std::uint32_t uw = extent->width;
    const std::uint32_t uh = extent->height;
    if (uw > limits.max_dimension || uh > limits.max_dimension) {
        return fail("image is " + std::to_string(uw) + "x" + std::to_string(uh) +
                    ", larger than the limit of " + std::to_string(limits.max_dimension) +
                    " per side");
    }
    if (encoded.size() - extent->data_offset < min_data_bytes(*extent)) {
        return fail("truncated pixel data (" + std::to_string(encoded.size() - extent->data_offset) +
                    " bytes for " + std::to_string(uw) + "x" + std::to_string(uh) + ")");
    }
    const std::uint64_t decoded = hdr_decoded_bytes(uw, uh);
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
        return fail("corrupt or unsupported .hdr header");
    }
    if (budget != nullptr && !budget->reserve(decoded)) {
        return fail("image would exceed the load's decoded-texture budget of " +
                    std::to_string(budget->total()) + " bytes");
    }

    int dw = 0;
    int dh = 0;
    float* pixels = stbi_loadf_from_memory(bytes, len, &dw, &dh, &channels, STBI_rgb);
    PixelBuffer owned = PixelBuffer::adopt(pixels, static_cast<std::size_t>(decoded), &stb_free);
    if (pixels == nullptr) return fail(".hdr pixel data is corrupt");
    if (dw != w || dh != h) return fail("decoded size differs from the header");

    HdrImage out;
    out.width = uw;
    out.height = uh;
    out.pixels = std::move(owned);
    return out;
}

std::expected<HdrImage, core::Error> load_radiance_hdr(const std::filesystem::path& path,
                                                       const TextureLimits& limits) {
    const std::string name = core::path_to_utf8(path);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return fail("cannot read '" + name + "'");
    if (size > limits.max_file_bytes) {
        return fail("'" + name + "' is " + std::to_string(size) +
                    " bytes, over the file size limit of " + std::to_string(limits.max_file_bytes));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("cannot open '" + name + "'");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (static_cast<std::uintmax_t>(in.gcount()) != size) return fail("short read of '" + name + "'");
    auto image = decode_radiance_hdr(bytes, limits);
    if (!image) return std::unexpected(core::Error{image.error().message + " ('" + name + "')"});
    return image;
}

} // namespace engine::asset
