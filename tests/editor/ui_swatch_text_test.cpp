// Colour swatch values (ROADMAP E8.4): the text on a swatch is dark on light
// colours and light on dark ones, by the swatch's relative luminance, so a
// white attenuation colour or a black emissive stay readable.

#ifdef ENGINE_EDITOR

#include <algorithm>

#include <gtest/gtest.h>

#include "editor/ui/theme.hpp"
#include "editor/ui/widgets.hpp"

namespace {

namespace ui = engine::editor::ui;

TEST(EditorUiSwatch, TextFollowsTheSwatchLuminance) {
    EXPECT_TRUE(ui::swatch_text_is_dark({1.0F, 1.0F, 1.0F, 1.0F})) << "white (attenuation default)";
    EXPECT_TRUE(ui::swatch_text_is_dark({0.95F, 0.78F, 0.45F, 1.0F})) << "gold base colour";
    EXPECT_TRUE(ui::swatch_text_is_dark({1.0F, 1.0F, 0.0F, 1.0F})) << "yellow";
    EXPECT_FALSE(ui::swatch_text_is_dark({0.0F, 0.0F, 0.0F, 1.0F})) << "black (emissive default)";
    EXPECT_FALSE(ui::swatch_text_is_dark({0.0F, 0.0F, 1.0F, 1.0F})) << "blue";
    EXPECT_FALSE(ui::swatch_text_is_dark({0.8F, 0.0F, 0.0F, 1.0F})) << "red";
    // HDR values are shown clamped: a bright HDR white is still white.
    EXPECT_TRUE(ui::swatch_text_is_dark({8.0F, 8.0F, 8.0F, 1.0F}));
    EXPECT_FALSE(ui::swatch_text_is_dark({-1.0F, -1.0F, -1.0F, 1.0F}));
}

TEST(EditorUiSwatch, ChosenTextHasTheBetterContrast) {
    constexpr ui::Rgba black{0.0F, 0.0F, 0.0F, 1.0F};
    constexpr ui::Rgba white{1.0F, 1.0F, 1.0F, 1.0F};
    for (int i = 0; i <= 20; ++i) {
        const float v = static_cast<float>(i) / 20.0F;
        for (const ui::Rgba c : {ui::Rgba{v, v, v, 1.0F}, ui::Rgba{v, 0.2F, 1.0F - v, 1.0F},
                                 ui::Rgba{0.1F, v, 0.3F, 1.0F}}) {
            const bool dark = ui::swatch_text_is_dark(c);
            const float with_black = ui::contrast_ratio(black, c);
            const float with_white = ui::contrast_ratio(white, c);
            EXPECT_GE(dark ? with_black : with_white, (dark ? with_white : with_black) - 0.05F)
                << c.r << " " << c.g << " " << c.b;
            EXPECT_GE(std::max(with_black, with_white), 4.5F) << "always WCAG AA";
        }
    }
}

} // namespace

#endif // ENGINE_EDITOR
