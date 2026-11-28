// Point <-> pixel conversions for high-density displays (fix/hidpi-rendering).

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "engine/core/pixel_scale.hpp"

namespace {

using engine::core::is_high_density_display;
using engine::core::PixelExtent;
using engine::core::pixels_to_points;
using engine::core::points_to_pixel_extent;
using engine::core::points_to_pixels;
using engine::core::sane_pixel_scale;

constexpr float k_nan = std::numeric_limits<float>::quiet_NaN();
constexpr float k_inf = std::numeric_limits<float>::infinity();

TEST(PixelScale, InvalidScalesCountAsOne) {
    EXPECT_FLOAT_EQ(sane_pixel_scale(2.0F), 2.0F);
    EXPECT_FLOAT_EQ(sane_pixel_scale(0.0F), 1.0F);
    EXPECT_FLOAT_EQ(sane_pixel_scale(-2.0F), 1.0F);
    EXPECT_FLOAT_EQ(sane_pixel_scale(k_nan), 1.0F);
    EXPECT_FLOAT_EQ(sane_pixel_scale(k_inf), 1.0F);
}

// The owner's report: a 490 x 392 point Viewport panel on a Retina display
// is 980 x 784 framebuffer pixels, and that is what the scene must render at.
TEST(PixelScale, RetinaPanelIsTwiceItsPointSize) {
    EXPECT_EQ(points_to_pixels(490.0F, 2.0F), 980U);
    EXPECT_EQ(points_to_pixels(392.0F, 2.0F), 784U);
    EXPECT_EQ(points_to_pixel_extent(490.0F, 392.0F, 2.0F, 2.0F, 16, 8192),
              (PixelExtent{980, 784}));
}

TEST(PixelScale, RoundsToTheNearestPixel) {
    EXPECT_EQ(points_to_pixels(245.5F, 2.0F), 491U); // not truncated to 490
    EXPECT_EQ(points_to_pixels(100.0F, 1.5F), 150U); // Windows at 150 %
    EXPECT_EQ(points_to_pixels(101.0F, 1.5F), 152U); // 151.5 rounds up
    EXPECT_EQ(points_to_pixels(333.0F, 1.25F), 416U); // 416.25
    EXPECT_EQ(points_to_pixels(0.3F, 1.0F), 0U);
}

TEST(PixelScale, DegenerateLengthsAreZero) {
    EXPECT_EQ(points_to_pixels(-10.0F, 2.0F), 0U);
    EXPECT_EQ(points_to_pixels(k_nan, 2.0F), 0U);
    EXPECT_EQ(points_to_pixels(100.0F, k_nan), 100U); // scale falls back to 1
    EXPECT_EQ(points_to_pixels(1e30F, 2.0F), 1048576U); // capped, no overflow
}

TEST(PixelScale, ExtentIsClampedPerAxis) {
    // A collapsed panel still yields a valid render target.
    EXPECT_EQ(points_to_pixel_extent(0.0F, 3.0F, 2.0F, 2.0F, 16, 8192), (PixelExtent{16, 16}));
    // A 5K panel at 2x is held at the device limit.
    EXPECT_EQ(points_to_pixel_extent(5000.0F, 100.0F, 2.0F, 2.0F, 16, 8192),
              (PixelExtent{8192, 200}));
    // Independent axis scales (ImGui reports one per axis).
    EXPECT_EQ(points_to_pixel_extent(100.0F, 100.0F, 2.0F, 1.0F, 1, 8192),
              (PixelExtent{200, 100}));
    // Odd sizes survive the round trip.
    EXPECT_EQ(points_to_pixel_extent(333.0F, 777.0F, 1.0F, 1.0F, 16, 8192),
              (PixelExtent{333, 777}));
    // min > max: max wins.
    EXPECT_EQ(points_to_pixel_extent(1.0F, 1.0F, 1.0F, 1.0F, 64, 32), (PixelExtent{32, 32}));
}

TEST(PixelScale, PixelsToPointsInvertsTheScale) {
    EXPECT_FLOAT_EQ(pixels_to_points(980.0F, 2.0F), 490.0F);
    EXPECT_FLOAT_EQ(pixels_to_points(150.0F, 1.5F), 100.0F);
    EXPECT_FLOAT_EQ(pixels_to_points(64.0F, 0.0F), 64.0F);
}

TEST(PixelScale, HighDensityThreshold) {
    EXPECT_FALSE(is_high_density_display(1.0F));
    EXPECT_FALSE(is_high_density_display(1.25F));
    EXPECT_TRUE(is_high_density_display(1.5F));
    EXPECT_TRUE(is_high_density_display(2.0F));
    EXPECT_FALSE(is_high_density_display(k_nan));
    EXPECT_FALSE(is_high_density_display(0.0F));
}

} // namespace
