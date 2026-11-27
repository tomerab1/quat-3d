// Coverage-preserving alpha mips (R12.11, issue #35): the CPU reference for
// alpha-tested (glTF MASK) textures. Each mip keeps level 0's alpha-test
// coverage, where a plain box filter loses thin features; colour is filtered
// with alpha weights and in linear light for sRGB data.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "engine/asset/alpha_coverage.hpp"
#include "support/alloc_probe.hpp"

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

// --- Runtime path: untrusted textures under the load's decode budget --------

// A decoded w x h MASK base colour (blades), as the PNG decoder hands it over,
// with its bytes claimed from `budget` like the decoder does.
a::TextureData decoded(std::uint32_t w, std::uint32_t h, a::DecodeBudget& budget) {
    a::TextureData t;
    t.width = w;
    t.height = h;
    t.color_space = a::TextureColorSpace::srgb;
    const std::vector<std::uint8_t> img = make_image(w, h, blades);
    EXPECT_TRUE(budget.reserve(img.size()));
    t.pixels = a::PixelBuffer::allocate(img.size());
    std::memcpy(t.pixels.data(), img.data(), img.size());
    return t;
}

TEST(AlphaCoverageBudget, CostCountsTheChainAndTwoFloatLevels) {
    const a::AlphaMipsCost c = a::alpha_tested_mips_cost(8, 4);
    // Chain 8x4, 4x2, 2x1, 1x1; peak float set = levels 1 + 2.
    EXPECT_EQ(c.chain_bytes, (32U + 8U + 2U + 1U) * 4U);
    EXPECT_EQ(c.working_bytes, (8U + 2U) * 16U);
    const a::AlphaMipsCost big = a::alpha_tested_mips_cost(8192, 8192);
    // Never a float copy of level 0: the working set stays at 1.25x level 0.
    EXPECT_LE(big.working_bytes, 8192ULL * 8192ULL * 4ULL * 5ULL / 4ULL);
}

// The fix for an untrusted 8k MASK texture: an over-budget chain is refused
// before anything large is allocated, and the image keeps its upload-time
// (blitted) mips.
TEST(AlphaCoverageBudget, OverBudgetIsRefusedBeforeAllocating) {
    constexpr std::uint32_t k = 1024;
    const a::AlphaMipsCost cost = a::alpha_tested_mips_cost(k, k);
    const std::uint64_t level0 = std::uint64_t{k} * k * 4;
    a::DecodeBudget budget(level0 + cost.chain_bytes + cost.working_bytes - 1);
    a::TextureData t = decoded(k, k, budget);
    const std::byte* before = t.pixels.data();
    bool applied = true;
    std::size_t largest = 0;
    {
        quat::test::AllocProbe probe;
        applied = a::apply_alpha_tested_mips(t, 0.5F, &budget);
        largest = probe.largest();
    }
    EXPECT_FALSE(applied);
    EXPECT_LT(largest, 64U * 1024U) << "allocated before the budget check";
    EXPECT_EQ(budget.used(), level0);
    EXPECT_TRUE(t.levels.empty());
    EXPECT_EQ(t.pixels.data(), before);
    // The per-image limit refuses it as well.
    a::DecodeBudget roomy(1ULL << 40);
    a::TextureData u = decoded(k, k, roomy);
    EXPECT_FALSE(a::apply_alpha_tested_mips(u, 0.5F, &roomy, cost.chain_bytes - 1));
    EXPECT_TRUE(u.levels.empty());
}

// Within budget: no float copy of level 0 (the largest allocation is one
// float level 1, the size of level 0 in RGBA8), the chain lands in the
// texture's own storage, and the load stays charged for the chain alone.
TEST(AlphaCoverageBudget, ChainIsBuiltInPlaceAndCharged) {
    constexpr std::uint32_t k = 512;
    const a::AlphaMipsCost cost = a::alpha_tested_mips_cost(k, k);
    const std::uint64_t level0 = std::uint64_t{k} * k * 4;
    a::DecodeBudget budget(level0 + cost.chain_bytes + cost.working_bytes);
    a::TextureData t = decoded(k, k, budget);
    const std::vector<std::uint8_t> original(
        reinterpret_cast<const std::uint8_t*>(t.pixels.data()),
        reinterpret_cast<const std::uint8_t*>(t.pixels.data()) + level0);
    std::size_t largest = 0;
    {
        quat::test::AllocProbe probe;
        ASSERT_TRUE(a::apply_alpha_tested_mips(t, 0.5F, &budget));
        largest = probe.largest();
    }
    EXPECT_LE(largest, level0);
    EXPECT_EQ(budget.used(), cost.chain_bytes);
    ASSERT_EQ(t.levels.size(), 10U);
    EXPECT_EQ(t.pixels.size(), cost.chain_bytes);
    EXPECT_EQ(std::memcmp(t.pixels.data(), original.data(), level0), 0);
    // Same chain as the reference builder.
    const auto reference = a::build_alpha_tested_mips(original, k, k, true, 0.5F);
    ASSERT_EQ(reference.size(), t.levels.size());
    for (std::size_t i = 0; i < reference.size(); ++i) {
        EXPECT_EQ(t.levels[i].width, reference[i].width);
        EXPECT_EQ(std::memcmp(t.pixels.data() + t.levels[i].offset, reference[i].pixels.data(),
                              reference[i].pixels.size()),
                  0)
            << "level " << i;
    }
}

} // namespace
