#pragma once
// Radiance RGBE (.hdr) encoder for tests: the decoder's input without
// depending on how the build compiled stb_image_write (the engine's copy is
// built with STBI_WRITE_NO_STDIO, which also leaves out its HDR writer).
//
// Format per Greg Ward, "Real Pixels", Graphics Gems II (1991), and the
// Radiance file format's "new-style" run-length encoding: scanlines of width
// 8..32767 are stored as the marker 2, 2, width (big-endian) followed by each
// of the four byte planes run-length encoded (a count byte > 128 is a run of
// count - 128 copies of the next byte; otherwise that many literal bytes).
// Other widths are stored flat (four bytes per pixel).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace quat::test {

// Ward's float -> shared-exponent conversion.
[[nodiscard]] inline std::array<std::uint8_t, 4> rgbe_from_rgb(const float* rgb) {
    const float v = std::max({rgb[0], rgb[1], rgb[2]});
    if (v < 1e-32F) return {0, 0, 0, 0};
    int e = 0;
    const float scale = std::frexp(v, &e) * 256.0F / v;
    return {static_cast<std::uint8_t>(rgb[0] * scale), static_cast<std::uint8_t>(rgb[1] * scale),
            static_cast<std::uint8_t>(rgb[2] * scale), static_cast<std::uint8_t>(e + 128)};
}

// Run-length encodes one byte plane of a scanline.
inline void rgbe_rle_plane(const std::vector<std::uint8_t>& plane, std::vector<std::byte>& out) {
    const auto put = [&](std::uint32_t b) { out.push_back(static_cast<std::byte>(b)); };
    std::size_t i = 0;
    while (i < plane.size()) {
        // A run worth encoding: at least 3 equal bytes (at most 127).
        std::size_t run = 1;
        while (i + run < plane.size() && run < 127 && plane[i + run] == plane[i]) ++run;
        if (run >= 3) {
            put(128U + static_cast<std::uint32_t>(run));
            put(plane[i]);
            i += run;
            continue;
        }
        // Literals up to the next run of 3 (at most 128).
        std::size_t n = 0;
        while (i + n < plane.size() && n < 128) {
            const std::size_t j = i + n;
            if (j + 2 < plane.size() && plane[j] == plane[j + 1] && plane[j] == plane[j + 2]) break;
            ++n;
        }
        put(static_cast<std::uint32_t>(n));
        for (std::size_t k = 0; k < n; ++k) put(plane[i + k]);
        i += n;
    }
}

// Encodes `w` x `h` RGB floats (row-major, top row first) as a .hdr file.
[[nodiscard]] inline std::vector<std::byte> encode_rgbe(int w, int h, const std::vector<float>& rgb) {
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(h) +
                               " +X " + std::to_string(w) + "\n";
    std::vector<std::byte> out;
    for (const char c : header) out.push_back(static_cast<std::byte>(c));
    const bool rle = w >= 8 && w < 32768;
    std::array<std::vector<std::uint8_t>, 4> planes;
    for (auto& p : planes) p.resize(static_cast<std::size_t>(w));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto px = rgbe_from_rgb(&rgb[static_cast<std::size_t>((y * w + x) * 3)]);
            if (!rle) {
                for (const std::uint8_t b : px) out.push_back(static_cast<std::byte>(b));
                continue;
            }
            for (std::size_t c = 0; c < 4; ++c) planes[c][static_cast<std::size_t>(x)] = px[c];
        }
        if (!rle) continue;
        for (const std::uint32_t b : {2U, 2U, static_cast<std::uint32_t>(w) >> 8U,
                                      static_cast<std::uint32_t>(w) & 0xFFU}) {
            out.push_back(static_cast<std::byte>(b));
        }
        for (const auto& p : planes) rgbe_rle_plane(p, out);
    }
    return out;
}

} // namespace quat::test
