// Render / output resolution text of the viewport stats, Stats panel and
// Profiler (fix/hidpi-rendering).

#include <gtest/gtest.h>

#include "editor/ui/resolution_text.hpp"

namespace {

using engine::editor::FrameResolution;
using engine::editor::ui::resolution_text;

TEST(UiResolutionText, NoFrameYet) {
    EXPECT_EQ(resolution_text(FrameResolution{}), "-");
    EXPECT_EQ(resolution_text(FrameResolution{100, 100, 0, 100}), "-");
}

TEST(UiResolutionText, NativeShowsOneSize) {
    EXPECT_EQ(resolution_text(FrameResolution{1960, 1568, 1960, 1568}), "1960 x 1568");
}

TEST(UiResolutionText, UpscaledShowsBothAndTheScale) {
    EXPECT_EQ(resolution_text(FrameResolution{1307, 1045, 1960, 1568}),
              "1307 x 1045 -> 1960 x 1568 (67%)");
    EXPECT_EQ(resolution_text(FrameResolution{980, 784, 1960, 1568}),
              "980 x 784 -> 1960 x 1568 (50%)");
}

} // namespace
