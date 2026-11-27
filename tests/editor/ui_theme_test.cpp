// Editor design tokens (ROADMAP E8.1): both themes keep every text/surface
// pairing the editor draws readable (WCAG AA), and the contrast maths match
// the WCAG reference values.

#ifdef ENGINE_EDITOR

#include <array>

#include <gtest/gtest.h>

#include "editor/ui/theme.hpp"

namespace {

namespace ui = engine::editor::ui;

TEST(EditorUiTheme, ContrastMathMatchesWcagReference) {
    const ui::Rgba black = ui::rgb(0x000000);
    const ui::Rgba white = ui::rgb(0xFFFFFF);
    EXPECT_NEAR(ui::relative_luminance(white), 1.0F, 1e-5F);
    EXPECT_NEAR(ui::relative_luminance(black), 0.0F, 1e-6F);
    EXPECT_NEAR(ui::contrast_ratio(white, black), 21.0F, 1e-3F);
    EXPECT_NEAR(ui::contrast_ratio(black, white), 21.0F, 1e-3F); // symmetric
    EXPECT_NEAR(ui::contrast_ratio(white, white), 1.0F, 1e-6F);
    // #767676 on white is the classic 4.54:1 AA boundary grey.
    EXPECT_NEAR(ui::contrast_ratio(ui::rgb(0x767676), white), 4.54F, 0.01F);
}

TEST(EditorUiTheme, TranslucentForegroundIsCompositedFirst) {
    const ui::Rgba bg = ui::rgb(0x000000);
    const ui::Rgba half_white = ui::rgb(0xFFFFFF, 0.5F);
    const ui::Rgba c = ui::composite(half_white, bg);
    EXPECT_NEAR(c.r, 0.5F, 1e-6F);
    EXPECT_FLOAT_EQ(c.a, 1.0F);
    EXPECT_LT(ui::contrast_ratio(half_white, bg), ui::contrast_ratio(ui::rgb(0xFFFFFF), bg));
}

void expect_readable(const ui::Theme& theme) {
    std::array<ui::ContrastPair, ui::k_max_contrast_pairs> pairs{};
    const std::size_t n = ui::contrast_pairs(theme, pairs);
    ASSERT_GE(n, 20U) << "every surface / role pairing is listed";
    ASSERT_LT(n, pairs.size()) << "k_max_contrast_pairs too small";
    for (std::size_t i = 0; i < n; ++i) {
        const ui::ContrastPair& p = pairs[i];
        const float ratio = ui::contrast_ratio(p.fg, p.bg);
        EXPECT_GE(ratio, p.min_ratio) << theme.name << ": " << p.what << " is " << ratio << ":1";
    }
}

TEST(EditorUiTheme, DarkThemeMeetsWcagAA) { expect_readable(ui::dark_theme()); }
TEST(EditorUiTheme, LightThemeMeetsWcagAA) { expect_readable(ui::light_theme()); }

TEST(EditorUiTheme, SurfacesStepInOneDirection) {
    // Dark: chrome darkest, each level lighter. Light: panels lighter than
    // chrome, inputs lightest. A hover (bg3) differs from the panel.
    const ui::Theme& d = ui::dark_theme();
    EXPECT_LT(ui::relative_luminance(d.bg0), ui::relative_luminance(d.bg1));
    EXPECT_LT(ui::relative_luminance(d.bg1), ui::relative_luminance(d.bg2));
    EXPECT_LT(ui::relative_luminance(d.bg2), ui::relative_luminance(d.bg3));
    const ui::Theme& l = ui::light_theme();
    EXPECT_LT(ui::relative_luminance(l.bg0), ui::relative_luminance(l.bg1));
    EXPECT_LT(ui::relative_luminance(l.bg1), ui::relative_luminance(l.bg2));
    EXPECT_NE(ui::relative_luminance(l.bg3), ui::relative_luminance(l.bg1));
}

TEST(EditorUiTheme, LookupByIdAndOpaqueSurfaces) {
    EXPECT_EQ(&ui::theme(ui::ThemeId::dark), &ui::dark_theme());
    EXPECT_EQ(&ui::theme(ui::ThemeId::light), &ui::light_theme());
    for (const ui::Theme* t : {&ui::dark_theme(), &ui::light_theme()}) {
        for (const ui::Rgba& c : {t->bg0, t->bg1, t->bg2, t->bg3, t->text, t->accent}) {
            EXPECT_FLOAT_EQ(c.a, 1.0F) << t->name;
        }
        EXPECT_LT(t->selection.a, 1.0F) << t->name << ": selection is a translucent tint";
    }
}

TEST(EditorUiTheme, HexHelper) {
    const ui::Rgba c = ui::rgb(0xFF8000, 0.25F);
    EXPECT_FLOAT_EQ(c.r, 1.0F);
    EXPECT_NEAR(c.g, 128.0F / 255.0F, 1e-6F);
    EXPECT_FLOAT_EQ(c.b, 0.0F);
    EXPECT_FLOAT_EQ(c.a, 0.25F);
}

} // namespace

#endif // ENGINE_EDITOR
