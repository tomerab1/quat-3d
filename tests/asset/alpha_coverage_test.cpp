// Coverage-preserving alpha mips (R12.11, issue #35): the CPU reference for
// alpha-tested (glTF MASK) textures. Each mip keeps level 0's alpha-test
// coverage, where a plain box filter loses thin features; colour is filtered
// with alpha weights and in linear light for sRGB data.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "engine/asset/alpha_coverage.hpp"

namespace {

namespace a = engine::asset;

// RGBA8 image with `alpha(x, y)` and white colour.
template <typename Fn>
std::vector<std::uint8_t> make_image(std::uint32_t w, std::uint32_t h, Fn&& alpha) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            out[(static_cast<std::size_t>(y) * w + x) * 4 + 3] = alpha(x, y);
        }
    }
    return out;
}

// Foliage-like mask: one-texel-wide "blades" (vertical lines every 4 texels)
// plus a sparse hashed speckle — thin features that box filtering erodes.
std::uint8_t blades(std::uint32_t x, std::uint32_t y) {
    const std::uint32_t hash = (x * 73856093U) ^ (y * 19349663U);
    return (x % 4 == 0 || hash % 11 == 0) ? 255 : 0;
}

float coverage(const a::MipLevelRgba8& level, float cutoff) {
    return a::alpha_test_coverage(level.pixels, level.width, level.height, cutoff);
}

TEST(AlphaCoverage, CoverageOfUniformAlpha) {
    const auto opaque = make_image(8, 8, [](auto, auto) { return std::uint8_t{255}; });
    const auto clear = make_image(8, 8, [](auto, auto) { return std::uint8_t{0}; });
    const auto half = make_image(8, 8, [](auto, auto) { return std::uint8_t{128}; });
    EXPECT_FLOAT_EQ(a::alpha_test_coverage(opaque, 8, 8, 0.5F), 1.0F);
    EXPECT_FLOAT_EQ(a::alpha_test_coverage(clear, 8, 8, 0.5F), 0.0F);
    EXPECT_FLOAT_EQ(a::alpha_test_coverage(half, 8, 8, 0.75F), 0.0F);
    EXPECT_FLOAT_EQ(a::alpha_test_coverage(half, 8, 8, 0.75F, 2.0F), 1.0F); // scaled
    EXPECT_FLOAT_EQ(a::alpha_test_coverage(half, 8, 9, 0.5F), 0.0F);        // mis-sized
}

TEST(AlphaCoverage, ChainHasVulkanMipExtents) {
    const auto img = make_image(300, 170, blades);
    const auto chain = a::build_alpha_tested_mips(img, 300, 170, true, 0.5F);
    ASSERT_EQ(chain.size(), 9U); // floor(log2(300)) + 1
    std::uint32_t w = 300;
    std::uint32_t h = 170;
    for (const a::MipLevelRgba8& level : chain) {
        EXPECT_EQ(level.width, w);
        EXPECT_EQ(level.height, h);
        EXPECT_EQ(level.pixels.size(), static_cast<std::size_t>(w) * h * 4);
        w = std::max(w / 2, 1U);
        h = std::max(h / 2, 1U);
    }
    EXPECT_EQ(chain.back().width, 1U);
    EXPECT_EQ(chain.back().height, 1U);
    EXPECT_EQ(chain.front().pixels, img); // level 0 unchanged
}

TEST(AlphaCoverage, PreservesLevelZeroCoverage) {
    constexpr float cutoff = 0.5F;
    const auto img = make_image(256, 256, blades);
    const auto preserved = a::build_alpha_tested_mips(img, 256, 256, false, cutoff);
    const auto plain = a::build_box_filtered_mips(img, 256, 256, false);
    ASSERT_EQ(preserved.size(), plain.size());
    const float target = coverage(preserved[0], cutoff);
    ASSERT_GT(target, 0.2F);
    ASSERT_LT(target, 0.5F);
    float worst_plain_error = 0.0F;
    for (std::size_t i = 1; i < preserved.size(); ++i) {
        if (preserved[i].width < 8) break; // a handful of texels cannot match closely
        EXPECT_NEAR(coverage(preserved[i], cutoff), target, 0.03F) << "level " << i;
        worst_plain_error =
            std::max(worst_plain_error, std::abs(coverage(plain[i], cutoff) - target));
    }
    // The box-filtered chain really loses the blades (the case the scaling fixes).
    EXPECT_GT(worst_plain_error, 0.15F);
}

TEST(AlphaCoverage, PreservesCoverageAtOtherCutoffs) {
    const auto img = make_image(128, 128, [](std::uint32_t x, std::uint32_t y) {
        // A soft radial falloff (round leaf) plus thin stems.
        const float dx = static_cast<float>(x) - 64.0F;
        const float dy = static_cast<float>(y) - 64.0F;
        const float r = std::sqrt(dx * dx + dy * dy) / 40.0F;
        const float a = x % 8 == 0 ? 1.0F : std::clamp(1.2F - r, 0.0F, 1.0F);
        return static_cast<std::uint8_t>(std::lround(a * 255.0F));
    });
    for (const float cutoff : {0.25F, 0.75F}) {
        const auto chain = a::build_alpha_tested_mips(img, 128, 128, true, cutoff);
        const float target = coverage(chain[0], cutoff);
        // Stems spanning whole texels pass or fail together: the reachable
        // coverage moves in steps of a few percent at the small levels.
        for (std::size_t i = 1; i < chain.size() && chain[i].width >= 8; ++i) {
            EXPECT_NEAR(coverage(chain[i], cutoff), target, 0.04F)
                << "cutoff " << cutoff << " level " << i;
        }
    }
}

// Transparent texels carry no colour: a green leaf on transparent black keeps
// its green in the mips instead of darkening toward the black around it.
TEST(AlphaCoverage, ColourIsAlphaWeighted) {
    const std::vector<std::uint8_t> img{0, 255, 0, 255, 0, 0, 0, 0, //
                                        0, 0, 0, 0, 0, 255, 0, 255};
    const auto chain = a::build_alpha_tested_mips(img, 2, 2, false, 0.5F);
    ASSERT_EQ(chain.size(), 2U);
    const auto& px = chain[1].pixels;
    EXPECT_EQ(px[0], 0);
    EXPECT_EQ(px[1], 255);
    EXPECT_EQ(px[2], 0);
}

// sRGB colour averages in linear light: black + white is mid-grey (188 in
// sRGB), not 128.
TEST(AlphaCoverage, SrgbAveragesInLinearLight) {
    const std::vector<std::uint8_t> img{0, 0, 0, 255, 255, 255, 255, 255};
    const auto srgb = a::build_alpha_tested_mips(img, 2, 1, true, 0.5F);
    const auto linear = a::build_alpha_tested_mips(img, 2, 1, false, 0.5F);
    ASSERT_EQ(srgb.size(), 2U);
    EXPECT_NEAR(srgb[1].pixels[0], 188, 1);
    EXPECT_NEAR(linear[1].pixels[0], 128, 1);
    EXPECT_EQ(srgb[1].pixels[3], 255); // fully covered stays opaque
}

// Odd extents spread the middle texel over both halves (area-weighted box).
TEST(AlphaCoverage, OddExtentsUseAreaWeights) {
    const std::vector<std::uint8_t> img{0, 0, 0, 255, 90, 90, 90, 255, 180, 180, 180, 255};
    const auto chain = a::build_box_filtered_mips(img, 3, 1, false);
    ASSERT_EQ(chain.size(), 2U);
    ASSERT_EQ(chain[1].width, 1U);
    EXPECT_EQ(chain[1].pixels[0], 90); // mean of all three
}

TEST(AlphaCoverage, RejectsMisSizedInput) {
    const std::vector<std::uint8_t> img(12, 255);
    EXPECT_TRUE(a::build_alpha_tested_mips(img, 2, 2, false, 0.5F).empty());
    EXPECT_TRUE(a::build_alpha_tested_mips({}, 0, 0, false, 0.5F).empty());
}

} // namespace
