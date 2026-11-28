#pragma once

// Editor sizing (ROADMAP E8.1): the 4 px spacing scale, corner radii, the
// type scale and the DPI maths. All sizes are logical pixels at 100 %; the
// helpers multiply by the current DPI scale (set by apply_theme /
// set_ui_scale), so panels never hard-code pixel values. Pure arithmetic —
// no ImGui — so tests can pin it down.

#include <algorithm>
#include <cmath>

namespace engine::editor::ui {

// ---- Spacing scale -------------------------------------------------------
// One unit is 4 px. space(n) is the n-th step: 4, 8, 12, 16, 20, 24, 32 ...
inline constexpr float k_space_unit = 4.0F;
[[nodiscard]] constexpr float space_px(int steps) { return k_space_unit * static_cast<float>(steps); }

// Named steps used across the editor (logical px at 100 %).
struct Spacing {
    static constexpr float xxs = space_px(1);  //  4: icon-to-label, inner gaps
    static constexpr float xs = space_px(2);   //  8: item spacing, frame padding x
    static constexpr float sm = space_px(3);   // 12: group spacing
    static constexpr float md = space_px(4);   // 16: panel padding, indents
    static constexpr float lg = space_px(6);   // 24: section breaks, empty states
    static constexpr float xl = space_px(8);   // 32: toolbar height, large gaps
};

// ---- Corner radii ----------------------------------------------------------
struct Radius {
    static constexpr float sm = 3.0F; // checkboxes, grab handles
    static constexpr float md = 4.0F; // frames, buttons, tabs
    static constexpr float lg = 6.0F; // popups, floating windows, toasts
};

// ---- Type scale ------------------------------------------------------------
struct TypeScale {
    static constexpr float body = 14.0F;    // Inter Regular: all UI text
    static constexpr float heading = 14.0F; // Inter SemiBold: headers, titles
    static constexpr float mono = 13.0F;    // JetBrains Mono: console, code, numbers
    static constexpr float icon = 15.0F;    // Lucide, merged into the fonts above
    static constexpr float icon_large = 18.0F; // toolbar / empty-state icons
};

// ---- Component sizes -------------------------------------------------------
struct Sizes {
    static constexpr float row_height = space_px(6);      // 24: property rows, list rows
    static constexpr float frame_pad_y = 4.0F;            // body 14 + 2 * 4 + borders ~ 24
    static constexpr float label_min = space_px(24);      // 96: property label column
    static constexpr float label_max = space_px(45);      // 180
    static constexpr float label_fraction = 0.40F;        // of the panel width
    static constexpr float toolbar_height = space_px(9);  // 36: main toolbar
    static constexpr float status_height = space_px(6);   // 24: status bar
    static constexpr float icon_button = space_px(7);     // 28: square icon buttons
    static constexpr float axis_marker = 3.0F;            // X/Y/Z stripe width
    static constexpr float toast_width = space_px(80);    // 320
    static constexpr float palette_width = space_px(150); // 600: command palette
};

// ---- DPI -------------------------------------------------------------------
// SDL3 reports a window's display scale (content scale x pixel density) and
// its pixel density separately. The UI is laid out in window coordinates, so
// sizes scale by the content scale alone; fonts additionally rasterise at the
// pixel density so they stay sharp on high-density (Retina) framebuffers.
struct DpiScale {
    float ui = 1.0F;     // multiplies every logical size
    float raster = 1.0F; // font rasteriser density (framebuffer px per window px)
};

inline constexpr float k_min_ui_scale = 0.5F;
inline constexpr float k_max_ui_scale = 4.0F;

// From SDL_GetWindowDisplayScale / SDL_GetWindowPixelDensity. Non-finite or
// non-positive inputs (no display yet) count as 1. The UI scale snaps to
// 1/8 steps so a 1.247 report does not produce blurry fractional layouts.
[[nodiscard]] inline DpiScale dpi_scale(float display_scale, float pixel_density) {
    const auto sane = [](float v) { return std::isfinite(v) && v > 0.0F ? v : 1.0F; };
    const float density = sane(pixel_density);
    float ui = sane(display_scale) / density;
    ui = std::clamp(std::round(ui * 8.0F) / 8.0F, k_min_ui_scale, k_max_ui_scale);
    return {ui, std::clamp(density, 1.0F, k_max_ui_scale)};
}

// Font pixel size for a logical size at `scale`, rounded to whole pixels
// (hinting and the glyph atlas want integral sizes).
[[nodiscard]] inline float font_px(float logical, float scale) {
    return std::max(1.0F, std::round(logical * scale));
}

// True when the fonts / style must be rebuilt for `next`.
[[nodiscard]] inline bool dpi_changed(DpiScale current, DpiScale next) {
    return std::fabs(current.ui - next.ui) > 1e-3F || std::fabs(current.raster - next.raster) > 1e-3F;
}

// The property grid's label column for a panel `avail` px wide at `scale`.
[[nodiscard]] inline float label_column_width(float avail, float scale) {
    return std::clamp(avail * Sizes::label_fraction, Sizes::label_min * scale,
                      Sizes::label_max * scale);
}

// The active DPI scale (set by apply_theme); scaled(px) = px * scale.
void set_ui_scale(float scale);
[[nodiscard]] float ui_scale();
[[nodiscard]] float scaled(float logical_px);

} // namespace engine::editor::ui
