#pragma once

// Point <-> pixel conversions for high-density displays.
//
// Windowing systems lay out windows and report pointer positions in
// "points" (macOS, Wayland) or pixels (Windows, X11); the framebuffer the GPU
// renders into is always in pixels. The ratio is the window's pixel density
// (SDL_GetWindowPixelDensity / ImGui's DisplayFramebufferScale): 2 on a
// Retina display with SDL_WINDOW_HIGH_PIXEL_DENSITY, 1 on Windows, where
// display scaling is a separate content scale and windows are sized in
// pixels. Everything that becomes a render-target size goes through these
// helpers so rounding and limits are the same everywhere.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::core {

struct PixelExtent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    friend bool operator==(const PixelExtent&, const PixelExtent&) = default;
};

// A scale factor as reported by the platform: non-finite or non-positive
// values (no display yet, a minimised window) count as 1.
[[nodiscard]] inline float sane_pixel_scale(float scale) {
    return std::isfinite(scale) && scale > 0.0F ? scale : 1.0F;
}

// Pixels covered by `points` at `scale`, rounded to the nearest whole pixel
// (a 245.5-point panel at 2x is 491 pixels, not 490). Negative and
// non-finite lengths are 0.
[[nodiscard]] inline std::uint32_t points_to_pixels(float points, float scale) {
    const float px = points * sane_pixel_scale(scale);
    if (!std::isfinite(px) || px <= 0.0F) return 0;
    constexpr float k_max = 1048576.0F; // far beyond any image dimension
    return static_cast<std::uint32_t>(std::lround(std::min(px, k_max)));
}

// The window-space (point) position of pixel coordinate `pixels`.
[[nodiscard]] inline float pixels_to_points(float pixels, float scale) {
    return pixels / sane_pixel_scale(scale);
}

// The pixel extent of a `width` x `height` point rectangle at a per-axis
// scale, each axis clamped to [min_dim, max_dim] (render targets need at
// least a few pixels and stay under the device's image limit).
[[nodiscard]] inline PixelExtent points_to_pixel_extent(float width, float height, float scale_x,
                                                        float scale_y, std::uint32_t min_dim,
                                                        std::uint32_t max_dim) {
    const std::uint32_t lo = std::min(min_dim, max_dim);
    return {std::clamp(points_to_pixels(width, scale_x), lo, max_dim),
            std::clamp(points_to_pixels(height, scale_y), lo, max_dim)};
}

// True when the display warrants upscaling by default: the window's display
// scale (pixel density x content scale, SDL_GetWindowDisplayScale) is 1.5 or
// more — a Retina Mac (2), a 4K monitor at 150-200 % on Windows or Wayland.
// Rendering every physical pixel there costs 2.25-4x a standard display.
inline constexpr float k_high_density_display_scale = 1.5F;
[[nodiscard]] inline bool is_high_density_display(float display_scale) {
    return sane_pixel_scale(display_scale) >= k_high_density_display_scale;
}

} // namespace engine::core
