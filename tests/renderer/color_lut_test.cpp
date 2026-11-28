// R20 look LUT strip size against the device's 2D image limit (PR #79
// review): Vulkan guarantees maxImageDimension2D >= 4096 only, and a 65^3
// LUT's strip is 65 * 65 = 4225 texels wide.

#include <gtest/gtest.h>

#include "engine/asset/cube_lut.hpp"
#include "engine/renderer/color_lut.hpp"

namespace {

namespace r = engine::renderer;
using engine::rhi::TextureCaps;

TextureCaps caps_with_limit(std::uint32_t max_dimension_2d) {
    TextureCaps caps;
    caps.max_dimension_2d = max_dimension_2d;
    return caps;
}

TEST(ColorLut, StripWiderThanTheDeviceLimitIsAPreciseError) {
    const TextureCaps min_spec = caps_with_limit(4096);
    EXPECT_TRUE(r::check_color_lut_fits(64, min_spec)) << "64 * 64 = 4096 fits exactly";
    const auto too_wide = r::check_color_lut_fits(65, min_spec);
    ASSERT_FALSE(too_wide);
    const std::string& msg = too_wide.error().message;
    EXPECT_NE(msg.find("4225"), std::string::npos) << msg;
    EXPECT_NE(msg.find("4096"), std::string::npos) << msg;
    EXPECT_NE(msg.find("size 64 or less"), std::string::npos) << msg;
}

TEST(ColorLut, LargestFittingSize) {
    EXPECT_EQ(r::max_color_lut_size(caps_with_limit(4096)), 64U);
    EXPECT_EQ(r::max_color_lut_size(caps_with_limit(16384)),
              engine::asset::CubeLut::k_max_size);
    // Unknown limit (0): every size the parser accepts is allowed.
    EXPECT_EQ(r::max_color_lut_size(TextureCaps{}), engine::asset::CubeLut::k_max_size);
    EXPECT_TRUE(r::check_color_lut_fits(engine::asset::CubeLut::k_max_size, TextureCaps{}));
}

} // namespace
