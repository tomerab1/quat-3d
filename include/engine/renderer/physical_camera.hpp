#pragma once

// Physical camera model (ROADMAP R20): exposure from aperture / shutter / ISO,
// field of view from focal length / sensor size, and the thin-lens circle of
// confusion that drives depth of field. Graphics-free; the passes take the
// values computed here, and the tests check them against closed forms.
//
// Exposure follows Lagarde & de Rousiers, "Moving Frostbite to Physically
// Based Rendering" (2014), section 5.1:
//   EV100    = log2(N^2 / t * 100 / S)
//   exposure = 1 / (1.2 * 2^EV100)      (saturation-based sensitivity, q = 0.65)
// so a scene luminance of 1.2 * 2^EV100 cd/m^2 maps to 1.0 before tonemapping.
// The values are photometric: they assume the lights are in photometric units
// (lux, candela). Scenes with arbitrary-unit lights use the exposure
// compensation (exposure.compensation_ev) to calibrate.

#include <glm/glm.hpp>

namespace engine::renderer {

// The lens and sensor of a camera. Distances in the units noted; the defaults
// are a full-frame (36 x 24 mm) camera at f/2.8, 1/50 s (a 180 degree shutter
// at 25 fps), ISO 100, focused at 10 m.
struct PhysicalCamera {
    float aperture_f       = 2.8F;        // f-number N
    float shutter_s        = 1.0F / 50.0F; // exposure time t (s)
    float iso              = 100.0F;      // sensitivity S
    float sensor_height_mm = 24.0F;       // vertical sensor size
    float focal_length_mm  = 0.0F;        // 0: derived from the vertical FOV
    float focus_distance_m = 10.0F;       // distance of the plane in focus
};

// Valid ranges (the settings table, the component codec and the editor clamp
// to these).
struct PhysicalCameraLimits {
    static constexpr float min_aperture = 0.7F, max_aperture = 64.0F;
    static constexpr float min_shutter = 1.0F / 16000.0F, max_shutter = 30.0F;
    static constexpr float min_iso = 25.0F, max_iso = 409600.0F;
    static constexpr float min_sensor_mm = 1.0F, max_sensor_mm = 100.0F;
    // 0 means "derive from the field of view"; anything else is >= min.
    static constexpr float min_focal_mm = 1.0F, max_focal_mm = 2000.0F;
    static constexpr float min_focus_m = 0.05F, max_focus_m = 100000.0F;
};

// Clamped copy (non-finite values fall back to the defaults).
[[nodiscard]] PhysicalCamera sanitized(const PhysicalCamera& camera);

// EV100 of an aperture / shutter / ISO triple.
[[nodiscard]] float ev100(float aperture_f, float shutter_s, float iso);
[[nodiscard]] float ev100(const PhysicalCamera& camera);
// Linear exposure multiplier of an EV100: 1 / (1.2 * 2^ev100).
[[nodiscard]] float exposure_from_ev100(float ev100);

// Vertical field of view (radians) of a focal length on a sensor, and back.
[[nodiscard]] float fov_from_focal_length(float focal_length_mm, float sensor_height_mm);
[[nodiscard]] float focal_length_from_fov(float fov_y, float sensor_height_mm);
// The focal length the camera images with: its own when set, else the one
// that gives `fov_y` on its sensor.
[[nodiscard]] float effective_focal_length_mm(const PhysicalCamera& camera, float fov_y);
// The vertical field of view the camera renders with: from the focal length
// when set, else `fov_y`.
[[nodiscard]] float effective_fov_y(const PhysicalCamera& camera, float fov_y);

// Thin-lens circle of confusion. For a point at view distance z (along the
// optical axis) with the lens focused at S:
//   c(z) = A * f / (S - f) * (z - S) / z,   A = f / N (aperture diameter)
// signed: negative in front of the focus plane (near field), positive behind
// it (far field), 0 on it; the far CoC tends to A f / (S - f) at infinity.
[[nodiscard]] float coc_diameter_mm(float depth_m, float focal_length_mm, float aperture_f,
                                    float focus_distance_m);

// The CoC as a signed radius in pixels of an image `image_height_px` tall:
// radius_px(z) = scale + bias / z (z in metres). The DoF passes evaluate this
// per pixel from linear depth.
struct CocParams {
    float scale = 0.0F; // radius at infinity (px)
    float bias  = 0.0F; // -scale * focus distance (px m)

    [[nodiscard]] float radius_px(float depth_m) const { return scale + bias / depth_m; }
};
[[nodiscard]] CocParams coc_params(const PhysicalCamera& camera, float fov_y,
                                   float image_height_px);

// Fraction of the frame interval the shutter is open (shutter angle / 360),
// which scales the per-frame velocity into the motion blur length. A shutter
// cannot stay open longer than a frame, so it is at most 1.
[[nodiscard]] float shutter_fraction(float shutter_s, float frame_dt_s);

} // namespace engine::renderer
