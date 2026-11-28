#pragma once

// Editor design tokens (ROADMAP E8.1): the semantic colour roles every panel
// draws with, a dark (default) and a light theme, and the WCAG contrast
// maths their tests hold them to. Panels never use raw colour literals —
// they read ui::tokens().
//
// Colours are sRGB-authored and blended in sRGB (gamma) space, as ImGui
// expects (the UI pass composites on the encoded bytes, issue #92), so
// contrast_ratio() and composite() follow WCAG 2.x directly on these values.
// See docs/editor-ux.md for how each role is meant to be used.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

struct ImVec4;

namespace engine::editor::ui {

struct Rgba {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
};

// 0xRRGGBB (opaque) -> Rgba.
[[nodiscard]] constexpr Rgba rgb(std::uint32_t hex, float alpha = 1.0F) {
    return {static_cast<float>((hex >> 16U) & 0xFFU) / 255.0F,
            static_cast<float>((hex >> 8U) & 0xFFU) / 255.0F,
            static_cast<float>(hex & 0xFFU) / 255.0F, alpha};
}
[[nodiscard]] constexpr Rgba with_alpha(Rgba c, float alpha) { return {c.r, c.g, c.b, alpha}; }

enum class ThemeId : std::uint8_t { dark, light };

struct Theme {
    ThemeId          id = ThemeId::dark;
    std::string_view name;

    // Surfaces, from the window chrome inwards: bg0 = menu / status / tab
    // bars and empty dock space, bg1 = panel body, bg2 = input fields and
    // section headers, bg3 = hovered / raised.
    Rgba bg0, bg1, bg2, bg3;
    Rgba text;           // primary text
    Rgba text_secondary; // labels in the property grid, hints, metadata
    Rgba text_disabled;  // inactive controls (no WCAG requirement; kept >= 3:1)
    Rgba accent;         // primary action / active state fill
    Rgba accent_hover;
    Rgba accent_active;
    Rgba on_accent;      // text and icons drawn on an accent fill
    Rgba accent_text;    // accent-coloured text / icons on bg0..bg2
    Rgba border;         // frame outlines, panel borders
    Rgba separator;      // hairlines between groups
    Rgba success, warning, error, info; // status text / icons on bg1
    Rgba selection;      // selected rows (translucent, over bg1)
    Rgba axis_x, axis_y, axis_z;        // X / Y / Z field markers and gizmo
};

[[nodiscard]] const Theme& dark_theme();
[[nodiscard]] const Theme& light_theme();
[[nodiscard]] const Theme& theme(ThemeId id);

// The active theme (dark until set_theme / apply_theme says otherwise).
[[nodiscard]] const Theme& tokens();

// WCAG 2.x relative luminance of an opaque sRGB colour.
[[nodiscard]] float relative_luminance(Rgba c);
// `fg` composited over the opaque `bg` (alpha blend in sRGB, as ImGui draws).
[[nodiscard]] Rgba composite(Rgba fg, Rgba bg);
// WCAG contrast ratio (1..21) of `fg` drawn over the opaque `bg`; a
// translucent `fg` is composited over `bg` first.
[[nodiscard]] float contrast_ratio(Rgba fg, Rgba bg);

// A text-on-surface pairing the themes must keep readable.
struct ContrastPair {
    std::string_view what;
    Rgba             fg;
    Rgba             bg;
    float            min_ratio; // 4.5 (WCAG AA body text) unless noted
};
// Every pairing the editor actually draws (text on each surface, status
// colours, text on accent and on selection); tests assert each meets
// min_ratio. Written into `out`, returns the count used.
std::size_t contrast_pairs(const Theme& t, std::span<ContrastPair> out);
inline constexpr std::size_t k_max_contrast_pairs = 32;

[[nodiscard]] ImVec4 to_imvec4(Rgba c);
[[nodiscard]] std::uint32_t to_u32(Rgba c); // ImU32 (IM_COL32 layout)

// Writes the theme's colours and the scaled metrics (ui/metrics.hpp) into
// the current ImGui style and makes it the active theme. `ui_scale` is the
// DPI scale (ui/metrics.hpp, dpi_scale); call again when either changes.
void apply_theme(const Theme& t, float ui_scale);

} // namespace engine::editor::ui
