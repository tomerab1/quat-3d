#pragma once

// Test-only reference decoders for the BC formats the KTX2 loader transcodes
// to (BC4 / BC5 / BC7), following the block layouts of the BC7 / RGTC
// specifications, so CPU tests can compare transcoded data with its source.
//
//   std::vector<std::uint8_t> rgba = quat::test::decode_bc(format, blocks, w, h);

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/asset/texture_data.hpp"
#include "support/bc7_tables.hpp"

namespace quat::test {

namespace bc_detail {

struct BitReader {
    const std::uint8_t* p;
    unsigned pos = 0;
    std::uint32_t read(unsigned n) {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i, ++pos) v |= ((p[pos >> 3] >> (pos & 7)) & 1U) << i;
        return v;
    }
};

// One BC4 block -> 16 single-channel values.
inline std::array<std::uint8_t, 16> bc4(const std::uint8_t* b) {
    const int r0 = b[0];
    const int r1 = b[1];
    std::array<int, 8> pal{r0, r1, 0, 0, 0, 0, 0, 0};
    if (r0 > r1) {
        for (int k = 1; k <= 6; ++k) pal[k + 1] = ((7 - k) * r0 + k * r1 + 3) / 7;
    } else {
        for (int k = 1; k <= 4; ++k) pal[k + 1] = ((5 - k) * r0 + k * r1 + 2) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
    std::array<std::uint8_t, 16> out{};
    for (int i = 0; i < 16; ++i) out[i] = static_cast<std::uint8_t>(pal[(bits >> (3 * i)) & 7]);
    return out;
}

struct Bc7Mode {
    unsigned subsets, partition_bits, rotation_bits, selector_bits, color_bits, alpha_bits,
        endpoint_pbits, shared_pbits, index_bits, index2_bits;
};
inline constexpr std::array<Bc7Mode, 8> k_modes{{{3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
                                                 {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
                                                 {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
                                                 {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
                                                 {1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
                                                 {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
                                                 {1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
                                                 {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}}};

inline int weight(unsigned bits, std::uint32_t index) {
    static constexpr std::array<int, 4> w2{0, 21, 43, 64};
    static constexpr std::array<int, 8> w3{0, 9, 18, 27, 37, 46, 55, 64};
    static constexpr std::array<int, 16> w4{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    return bits == 2 ? w2[index] : bits == 3 ? w3[index] : w4[index];
}

// One BC7 block -> 16 RGBA texels.
inline std::array<std::uint8_t, 64> bc7(const std::uint8_t* b) {
    std::array<std::uint8_t, 64> out{};
    unsigned mode = 0;
    while (mode < 8 && ((b[0] >> mode) & 1U) == 0) ++mode;
    if (mode == 8) return out; // reserved: decodes to transparent black
    const Bc7Mode& m = k_modes[mode];
    BitReader r{b};
    r.read(mode + 1);
    const std::uint32_t part = r.read(m.partition_bits);
    const std::uint32_t rotation = r.read(m.rotation_bits);
    const std::uint32_t selector = r.read(m.selector_bits);
    const unsigned endpoints = m.subsets * 2;
    std::array<std::array<int, 4>, 6> ep{};
    for (unsigned c = 0; c < 3; ++c) {
        for (unsigned e = 0; e < endpoints; ++e) ep[e][c] = static_cast<int>(r.read(m.color_bits));
    }
    for (unsigned e = 0; e < endpoints && m.alpha_bits > 0; ++e) {
        ep[e][3] = static_cast<int>(r.read(m.alpha_bits));
    }
    unsigned cbits = m.color_bits;
    unsigned abits = m.alpha_bits;
    if (m.endpoint_pbits != 0 || m.shared_pbits != 0) {
        std::array<std::uint32_t, 6> p{};
        if (m.endpoint_pbits != 0) {
            for (unsigned e = 0; e < endpoints; ++e) p[e] = r.read(1);
        } else {
            for (unsigned s = 0; s < m.subsets; ++s) p[2 * s] = p[2 * s + 1] = r.read(1);
        }
        for (unsigned e = 0; e < endpoints; ++e) {
            for (unsigned c = 0; c < 4; ++c) ep[e][c] = (ep[e][c] << 1) | static_cast<int>(p[e]);
        }
        ++cbits;
        if (abits != 0) ++abits;
    }
    for (unsigned e = 0; e < endpoints; ++e) {
        for (unsigned c = 0; c < 4; ++c) {
            const unsigned bits = c < 3 ? cbits : abits;
            if (bits == 0) {
                ep[e][c] = 255;
                continue;
            }
            ep[e][c] = (ep[e][c] << (8 - bits)) | (ep[e][c] >> (2 * bits - 8));
        }
    }
    const auto subset_of = [&](unsigned i) -> unsigned {
        if (m.subsets == 2) return bc7::k_partition2[part * 16 + i];
        if (m.subsets == 3) return bc7::k_partition3[part * 16 + i];
        return 0;
    };
    const auto anchor = [&](unsigned i) {
        if (i == 0) return true;
        if (m.subsets == 2) return i == bc7::k_anchor2[part];
        if (m.subsets == 3) return i == bc7::k_anchor3_1[part] || i == bc7::k_anchor3_2[part];
        return false;
    };
    std::array<std::uint32_t, 16> idx{};
    std::array<std::uint32_t, 16> idx2{};
    for (unsigned i = 0; i < 16; ++i) idx[i] = r.read(m.index_bits - (anchor(i) ? 1 : 0));
    for (unsigned i = 0; i < 16 && m.index2_bits > 0; ++i) {
        idx2[i] = r.read(m.index2_bits - (i == 0 ? 1 : 0));
    }
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned s = subset_of(i);
        const auto& e0 = ep[2 * s];
        const auto& e1 = ep[2 * s + 1];
        unsigned cb = m.index_bits;
        unsigned ab = m.index_bits;
        std::uint32_t ci = idx[i];
        std::uint32_t ai = idx[i];
        if (m.index2_bits > 0) {
            if (selector == 0) {
                ab = m.index2_bits;
                ai = idx2[i];
            } else {
                cb = m.index2_bits;
                ci = idx2[i];
            }
        }
        std::array<int, 4> px{};
        for (unsigned c = 0; c < 4; ++c) {
            const int w = c < 3 ? weight(cb, ci) : weight(ab, ai);
            px[c] = ((64 - w) * e0[c] + w * e1[c] + 32) >> 6;
        }
        if (rotation != 0) std::swap(px[3], px[rotation - 1]);
        for (unsigned c = 0; c < 4; ++c) out[i * 4 + c] = static_cast<std::uint8_t>(px[c]);
    }
    return out;
}

} // namespace bc_detail

// Decodes level data of `format` (bc4 / bc5 / bc7) to tightly packed RGBA8:
// BC4 fills R (G = B = 0), BC5 fills R and G, alpha is 255 unless BC7.
inline std::vector<std::uint8_t> decode_bc(engine::asset::TexelFormat format,
                                           std::span<const std::byte> data, std::uint32_t width,
                                           std::uint32_t height) {
    using engine::asset::TexelFormat;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4, 0);
    const std::size_t block_bytes = format == TexelFormat::bc4 ? 8 : 16;
    const std::uint32_t bw = (width + 3) / 4;
    const std::uint32_t bh = (height + 3) / 4;
    if (data.size() < static_cast<std::size_t>(bw) * bh * block_bytes) return {};
    for (std::uint32_t by = 0; by < bh; ++by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            const auto* b = reinterpret_cast<const std::uint8_t*>(data.data()) +
                            (static_cast<std::size_t>(by) * bw + bx) * block_bytes;
            std::array<std::uint8_t, 64> px{};
            if (format == TexelFormat::bc7) {
                px = bc_detail::bc7(b);
            } else {
                const auto r = bc_detail::bc4(b);
                const auto g = format == TexelFormat::bc5 ? bc_detail::bc4(b + 8) : decltype(r){};
                for (int i = 0; i < 16; ++i) {
                    px[i * 4] = r[i];
                    px[i * 4 + 1] = g[i];
                    px[i * 4 + 3] = 255;
                }
            }
            for (std::uint32_t i = 0; i < 16; ++i) {
                const std::uint32_t x = bx * 4 + i % 4;
                const std::uint32_t y = by * 4 + i / 4;
                if (x >= width || y >= height) continue;
                for (int c = 0; c < 4; ++c) {
                    out[(static_cast<std::size_t>(y) * width + x) * 4 + c] = px[i * 4 + c];
                }
            }
        }
    }
    return out;
}

} // namespace quat::test
