// Editor sizing (ROADMAP E8.1): the 4 px spacing scale, the DPI maths and
// the property grid's label column.

#ifdef ENGINE_EDITOR

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "editor/ui/metrics.hpp"

namespace {

namespace ui = engine::editor::ui;

TEST(EditorUiMetrics, SpacingIsOnTheFourPixelGrid) {
    EXPECT_FLOAT_EQ(ui::space_px(1), 4.0F);
    EXPECT_FLOAT_EQ(ui::space_px(6), 24.0F);
    for (const float s : {ui::Spacing::xxs, ui::Spacing::xs, ui::Spacing::sm, ui::Spacing::md,
                          ui::Spacing::lg, ui::Spacing::xl, ui::Sizes::row_height,
                          ui::Sizes::toolbar_height, ui::Sizes::status_height,
                          ui::Sizes::icon_button}) {
        EXPECT_FLOAT_EQ(std::fmod(s, ui::k_space_unit), 0.0F) << s;
    }
    EXPECT_LT(ui::Spacing::xxs, ui::Spacing::xs);
    EXPECT_LT(ui::Spacing::xs, ui::Spacing::sm);
    EXPECT_LT(ui::Spacing::sm, ui::Spacing::md);
    EXPECT_LT(ui::Spacing::md, ui::Spacing::lg);
    EXPECT_LT(ui::Spacing::lg, ui::Spacing::xl);
}

TEST(EditorUiMetrics, RowHeightFitsBodyTextWithPadding) {
    // A frame is the body font plus vertical padding on both sides.
    EXPECT_LE(ui::TypeScale::body + 2.0F * ui::Sizes::frame_pad_y, ui::Sizes::row_height);
    EXPECT_LT(ui::Radius::sm, ui::Radius::md);
    EXPECT_LT(ui::Radius::md, ui::Radius::lg);
}

TEST(EditorUiMetrics, DpiScaleSeparatesContentScaleFromPixelDensity) {
    // 100 %: nothing to scale.
    ui::DpiScale s = ui::dpi_scale(1.0F, 1.0F);
    EXPECT_FLOAT_EQ(s.ui, 1.0F);
    EXPECT_FLOAT_EQ(s.raster, 1.0F);
    // Windows at 150 %: window coordinates are physical pixels.
    s = ui::dpi_scale(1.5F, 1.0F);
    EXPECT_FLOAT_EQ(s.ui, 1.5F);
    EXPECT_FLOAT_EQ(s.raster, 1.0F);
    // macOS Retina: display scale 2 is all pixel density; the layout stays
    // at 1x in window points, fonts rasterise at 2x.
    s = ui::dpi_scale(2.0F, 2.0F);
    EXPECT_FLOAT_EQ(s.ui, 1.0F);
    EXPECT_FLOAT_EQ(s.raster, 2.0F);
    // Wayland 300 % on a density-2 output: 1.5x layout, 2x raster.
    s = ui::dpi_scale(3.0F, 2.0F);
    EXPECT_FLOAT_EQ(s.ui, 1.5F);
    EXPECT_FLOAT_EQ(s.raster, 2.0F);
}

TEST(EditorUiMetrics, DpiScaleSnapsAndClamps) {
    EXPECT_FLOAT_EQ(ui::dpi_scale(1.247F, 1.0F).ui, 1.25F); // 1/8 steps
    EXPECT_FLOAT_EQ(ui::dpi_scale(1.06F, 1.0F).ui, 1.0F);
    EXPECT_FLOAT_EQ(ui::dpi_scale(0.1F, 1.0F).ui, ui::k_min_ui_scale);
    EXPECT_FLOAT_EQ(ui::dpi_scale(40.0F, 1.0F).ui, ui::k_max_ui_scale);
    // No display yet (SDL returns 0) or garbage: treat as 100 %.
    EXPECT_FLOAT_EQ(ui::dpi_scale(0.0F, 0.0F).ui, 1.0F);
    EXPECT_FLOAT_EQ(ui::dpi_scale(std::numeric_limits<float>::quiet_NaN(), 1.0F).ui, 1.0F);
    EXPECT_FLOAT_EQ(ui::dpi_scale(1.0F, -2.0F).raster, 1.0F);
}

TEST(EditorUiMetrics, FontSizesRoundToWholePixels) {
    EXPECT_FLOAT_EQ(ui::font_px(14.0F, 1.0F), 14.0F);
    EXPECT_FLOAT_EQ(ui::font_px(14.0F, 1.25F), 18.0F); // 17.5 rounds up
    EXPECT_FLOAT_EQ(ui::font_px(13.0F, 1.5F), 20.0F);  // 19.5
    EXPECT_FLOAT_EQ(ui::font_px(14.0F, 2.0F), 28.0F);
    EXPECT_FLOAT_EQ(ui::font_px(0.1F, 1.0F), 1.0F);    // never zero
}

TEST(EditorUiMetrics, RebuildOnlyWhenTheScaleChanges) {
    EXPECT_FALSE(ui::dpi_changed({1.0F, 1.0F}, {1.0F, 1.0F}));
    EXPECT_TRUE(ui::dpi_changed({1.0F, 1.0F}, {1.25F, 1.0F}));
    EXPECT_TRUE(ui::dpi_changed({1.0F, 1.0F}, {1.0F, 2.0F}));
}

TEST(EditorUiMetrics, LabelColumnFollowsThePanelWithinBounds) {
    // Narrow panel: the minimum; wide: the maximum; between: a fraction.
    EXPECT_FLOAT_EQ(ui::label_column_width(100.0F, 1.0F), ui::Sizes::label_min);
    EXPECT_FLOAT_EQ(ui::label_column_width(2000.0F, 1.0F), ui::Sizes::label_max);
    EXPECT_FLOAT_EQ(ui::label_column_width(300.0F, 1.0F), 300.0F * ui::Sizes::label_fraction);
    // The bounds scale with DPI.
    EXPECT_FLOAT_EQ(ui::label_column_width(2000.0F, 2.0F), ui::Sizes::label_max * 2.0F);
}

} // namespace

#endif // ENGINE_EDITOR
