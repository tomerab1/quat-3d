// R20 look LUT texels (PR #79 review): the table is uploaded as a size^3 3D
// image (maxImageDimension3D >= 256 on every device, so a 65^3 Resolve export
// fits everywhere), red fastest, then green, then blue, as RGBA16F.

#include <cstddef>
#include <cstdint>

#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>

#include "engine/asset/cube_lut.hpp"
#include "engine/renderer/color_lut.hpp"

namespace {

namespace r = engine::renderer;
using engine::asset::CubeLut;

// A table whose entry (r, g, b) holds distinct values per axis.
CubeLut indexed_lut(std::uint32_t n) {
    CubeLut lut;
    lut.size = n;
    lut.rgb.resize(static_cast<std::size_t>(n) * n * n * 3);
    for (std::uint32_t b = 0; b < n; ++b) {
        for (std::uint32_t g = 0; g < n; ++g) {
            for (std::uint32_t x = 0; x < n; ++x) {
                const std::size_t i = ((static_cast<std::size_t>(b) * n + g) * n + x) * 3;
                lut.rgb[i + 0] = static_cast<float>(x) * 0.125F;
                lut.rgb[i + 1] = static_cast<float>(g) * 0.25F;
                lut.rgb[i + 2] = static_cast<float>(b) * 0.5F;
            }
        }
    }
    return lut;
}

float texel(const std::vector<std::uint16_t>& t, std::uint32_t n, std::uint32_t x, std::uint32_t y,
            std::uint32_t z, int channel) {
    const std::size_t i = ((static_cast<std::size_t>(z) * n + y) * n + x) * 4 +
                          static_cast<std::size_t>(channel);
    return glm::unpackHalf1x16(t[i]);
}

TEST(ColorLut, TexelsAreRedFastestThenGreenThenBlue) {
    constexpr std::uint32_t n = 3;
    const std::vector<std::uint16_t> t = r::color_lut_texels(indexed_lut(n));
    ASSERT_EQ(t.size(), static_cast<std::size_t>(n) * n * n * 4);
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t y = 0; y < n; ++y) {
            for (std::uint32_t x = 0; x < n; ++x) {
                EXPECT_FLOAT_EQ(texel(t, n, x, y, z, 0), static_cast<float>(x) * 0.125F);
                EXPECT_FLOAT_EQ(texel(t, n, x, y, z, 1), static_cast<float>(y) * 0.25F);
                EXPECT_FLOAT_EQ(texel(t, n, x, y, z, 2), static_cast<float>(z) * 0.5F);
                EXPECT_FLOAT_EQ(texel(t, n, x, y, z, 3), 1.0F);
            }
        }
    }
}

TEST(ColorLut, LargestTableFitsEveryDevicesThreeDimensionalLimit) {
    // maxImageDimension3D is at least 256 (Vulkan 1.3 required limits).
    EXPECT_LE(CubeLut::k_max_size, 256U);
    const std::vector<std::uint16_t> t = r::color_lut_texels(indexed_lut(CubeLut::k_max_size));
    EXPECT_EQ(t.size(), static_cast<std::size_t>(65) * 65 * 65 * 4);
}

} // namespace
