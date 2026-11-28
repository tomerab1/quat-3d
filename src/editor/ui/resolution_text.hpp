#pragma once

// Text for the scene's render and output resolutions (viewport stats
// overlay, Stats panel, Profiler). Pure, so it is unit-tested without ImGui.

#include <cstdio>
#include <string>

#include "engine/editor/editor.hpp"

namespace engine::editor::ui {

// "1960 x 1568" (output only, when the scene renders at native resolution)
// or "1307 x 1045 -> 1960 x 1568 (67%)" when it is upscaled; the percentage
// is the per-axis render scale. "-" before the first frame.
[[nodiscard]] inline std::string resolution_text(const FrameResolution& r) {
    if (r.output_width == 0 || r.output_height == 0) return "-";
    char buf[96];
    if (r.render_width == r.output_width && r.render_height == r.output_height) {
        std::snprintf(buf, sizeof(buf), "%u x %u", r.output_width, r.output_height);
        return buf;
    }
    const double pct =
        100.0 * static_cast<double>(r.render_width) / static_cast<double>(r.output_width);
    std::snprintf(buf, sizeof(buf), "%u x %u -> %u x %u (%.0f%%)", r.render_width,
                  r.render_height, r.output_width, r.output_height, pct);
    return buf;
}

} // namespace engine::editor::ui
