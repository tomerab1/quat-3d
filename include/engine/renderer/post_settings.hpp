#pragma once

// Settings of the cinematic post stack (ROADMAP R20): depth of field, motion
// blur, the tonemap operator, colour grading and film / lens effects. Members
// of RenderSettings (render_settings.hpp), listed in its descriptor table.
// The lens itself (aperture, shutter, ISO, focal length, focus distance) is
// per camera: scene::Camera (PhysicalCamera in physical_camera.hpp).

#include <array>
#include <string>
#include <string_view>

namespace engine::renderer {

// Gather-based bokeh depth of field (DofPass). The circle of confusion comes
// from the active camera's lens; these bound its cost.
struct DofSettings {
    bool  enabled       = false;
    float max_radius_px = 24.0F; // largest CoC radius, display pixels
    int   rings         = 4;     // gather rings (samples: 1 + 4 r (r + 1) per field)
};

// McGuire 2012 reconstruction-filter motion blur (MotionBlurPass). Blur
// length = velocity x the camera's shutter fraction (shutter angle / 360).
// Off by default like depth of field: the default 1/50 s shutter is a
// 360-degree shutter at 60 fps, so turning it on changes a scene's look.
struct MotionBlurSettings {
    bool  enabled       = false;
    float max_radius_px = 32.0F; // largest blur radius (also the tile size), display px
    int   samples       = 12;    // reconstruction taps per pixel
    float scale         = 1.0F;  // artistic multiplier on the shutter fraction
};

// Tonemap operators (TonemapPass): Narkowicz's ACES fit (the engine's
// original), AgX (Sobotka, as Blender 4 ships it) and Khronos PBR Neutral.
enum class TonemapOperator : int { aces = 0, agx = 1, pbr_neutral = 2 };
inline constexpr std::array<std::string_view, 3> tonemap_operator_names = {"aces", "agx",
                                                                           "pbr_neutral"};

struct TonemapSettings {
    int op = static_cast<int>(TonemapOperator::aces); // TonemapOperator
};

// Scene-referred colour grading, applied before the tonemap: white balance
// (von Kries in LMS), then contrast, lift / gamma / gain (per channel) and
// saturation in ACEScct; a display-referred `.cube` look LUT after the
// tonemap. The defaults are an exact no-op.
struct GradingSettings {
    float temperature_k = 6500.0F; // white balance: light colour rendered neutral
    float tint          = 0.0F;    // green (-) / magenta (+) along the isotherm
    float contrast      = 1.0F;    // around ACEScct mid-grey
    float saturation    = 1.0F;
    float lift_r = 0.0F, lift_g = 0.0F, lift_b = 0.0F;
    float gamma_r = 1.0F, gamma_g = 1.0F, gamma_b = 1.0F;
    float gain_r = 1.0F, gain_g = 1.0F, gain_b = 1.0F;
    std::string lut;                  // project-relative .cube file, "" = none
    float       lut_contribution = 1.0F; // blend toward the LUT's output

    // True when every scene-referred control is at its neutral value (the
    // tonemap pass then skips the ACEScct round trip).
    [[nodiscard]] bool scene_neutral() const {
        return temperature_k == 6500.0F && tint == 0.0F && contrast == 1.0F &&
               saturation == 1.0F && lift_r == 0.0F && lift_g == 0.0F && lift_b == 0.0F &&
               gamma_r == 1.0F && gamma_g == 1.0F && gamma_b == 1.0F && gain_r == 1.0F &&
               gain_g == 1.0F && gain_b == 1.0F;
    }
};

// Lens and film effects of the final image. The natural vignette is the
// cos^4 illumination falloff of the active camera's field of view; the rest
// are artistic and off by default.
struct FilmSettings {
    float vignette_natural    = 0.0F; // 0 none .. 1 full cos^4 law
    float vignette_intensity  = 0.0F; // artistic darkening at the corners
    float vignette_smoothness = 0.5F; // width of the artistic falloff
    float grain_intensity     = 0.0F; // film grain strength (display-referred)
    float grain_size_px       = 1.5F; // grain size
    float chromatic_aberration = 0.0F; // lateral CA at the image corners
};

} // namespace engine::renderer
