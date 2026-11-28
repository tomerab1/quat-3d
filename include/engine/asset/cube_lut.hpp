#pragma once

// `.cube` 3D colour lookup tables (Adobe / Resolve text format; ROADMAP R20
// look LUTs). Files are untrusted input: the parser bounds the file size and
// the table size, accepts only the keywords below, requires exactly size^3
// finite entries and never reads past the text.
//
//   # comment                 TITLE "name"
//   LUT_3D_SIZE N             (2..65)
//   DOMAIN_MIN r g b          DOMAIN_MAX r g b        (default 0 0 0 / 1 1 1)
//   LUT_3D_INPUT_RANGE lo hi  (Resolve's form of the domain)
//   r g b                     N^3 lines, red changing fastest, then green, blue
//
// 1D LUTs (LUT_1D_SIZE) are rejected.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/error.hpp"

namespace engine::asset {

struct CubeLut {
    static constexpr std::uint32_t k_min_size = 2;
    static constexpr std::uint32_t k_max_size = 65;
    static constexpr std::size_t   k_max_file_bytes = 16U << 20U; // 65^3 lines fit easily

    std::uint32_t        size = 0;
    std::array<float, 3> domain_min{0.0F, 0.0F, 0.0F};
    std::array<float, 3> domain_max{1.0F, 1.0F, 1.0F};
    std::string          title;
    // size^3 RGB triples, index (b * size + g) * size + r.
    std::vector<float> rgb;

    [[nodiscard]] std::array<float, 3> at(std::uint32_t r, std::uint32_t g, std::uint32_t b) const {
        const std::size_t i = (static_cast<std::size_t>(b) * size + g) * size + r;
        return {rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]};
    }
};

// Parses `.cube` text. The error names the line and what is wrong.
[[nodiscard]] std::expected<CubeLut, core::Error> parse_cube_lut(std::string_view text);

// Reads and parses a file (at most CubeLut::k_max_file_bytes).
[[nodiscard]] std::expected<CubeLut, core::Error> load_cube_lut(const std::filesystem::path& path);

// The identity table of `size` (output = input over the [0, 1] domain).
[[nodiscard]] CubeLut identity_cube_lut(std::uint32_t size);

} // namespace engine::asset
