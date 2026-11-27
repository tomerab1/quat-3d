#include "engine/renderer/physical_camera.hpp"

#include <algorithm>
#include <cmath>

namespace engine::renderer {

namespace {

using L = PhysicalCameraLimits;

[[nodiscard]] float clamp_or(float v, float lo, float hi, float fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

// A thin lens cannot focus closer than its focal length; keep the focus
// distance a hair beyond it so f / (S - f) stays finite.
[[nodiscard]] float focus_mm(float focus_distance_m, float focal_length_mm) {
    return std::max(focus_distance_m * 1000.0F, focal_length_mm * 1.001F + 1e-3F);
}

} // namespace

PhysicalCamera sanitized(const PhysicalCamera& c) {
    const PhysicalCamera d{};
    PhysicalCamera out;
    out.aperture_f = clamp_or(c.aperture_f, L::min_aperture, L::max_aperture, d.aperture_f);
    out.shutter_s = clamp_or(c.shutter_s, L::min_shutter, L::max_shutter, d.shutter_s);
    out.iso = clamp_or(c.iso, L::min_iso, L::max_iso, d.iso);
    out.sensor_height_mm =
        clamp_or(c.sensor_height_mm, L::min_sensor_mm, L::max_sensor_mm, d.sensor_height_mm);
    out.focal_length_mm = clamp_or(c.focal_length_mm, 0.0F, L::max_focal_mm, 0.0F);
    out.focus_distance_m =
        clamp_or(c.focus_distance_m, L::min_focus_m, L::max_focus_m, d.focus_distance_m);
    return out;
}

float ev100(float aperture_f, float shutter_s, float iso) {
    return std::log2(aperture_f * aperture_f / shutter_s * 100.0F / iso);
}

float ev100(const PhysicalCamera& camera) {
    const PhysicalCamera c = sanitized(camera);
    return ev100(c.aperture_f, c.shutter_s, c.iso);
}

float exposure_from_ev100(float ev) { return 1.0F / (1.2F * std::exp2(ev)); }

float fov_from_focal_length(float focal_length_mm, float sensor_height_mm) {
    return 2.0F * std::atan(0.5F * sensor_height_mm / focal_length_mm);
}

float focal_length_from_fov(float fov_y, float sensor_height_mm) {
    return 0.5F * sensor_height_mm / std::tan(0.5F * fov_y);
}

float effective_focal_length_mm(const PhysicalCamera& camera, float fov_y) {
    const PhysicalCamera c = sanitized(camera);
    if (c.focal_length_mm > 0.0F) return c.focal_length_mm;
    const float fov = std::isfinite(fov_y) ? std::clamp(fov_y, 1e-3F, 3.1F) : 1.0471976F;
    return focal_length_from_fov(fov, c.sensor_height_mm);
}

float effective_fov_y(const PhysicalCamera& camera, float fov_y) {
    const PhysicalCamera c = sanitized(camera);
    if (c.focal_length_mm > 0.0F) return fov_from_focal_length(c.focal_length_mm, c.sensor_height_mm);
    return fov_y;
}

float coc_diameter_mm(float depth_m, float focal_length_mm, float aperture_f,
                      float focus_distance_m) {
    const float f = focal_length_mm;
    const float s = focus_mm(focus_distance_m, f);
    const float z = depth_m * 1000.0F;
    return f * f / (aperture_f * (s - f)) * (z - s) / z;
}

CocParams coc_params(const PhysicalCamera& camera, float fov_y, float image_height_px) {
    const PhysicalCamera c = sanitized(camera);
    const float f = effective_focal_length_mm(c, fov_y);
    const float s = focus_mm(c.focus_distance_m, f);
    // Diameter at infinity on the sensor, as a radius in pixels.
    const float far_mm = f * f / (c.aperture_f * (s - f));
    CocParams out;
    out.scale = 0.5F * far_mm / c.sensor_height_mm * image_height_px;
    out.bias = -out.scale * (s / 1000.0F);
    return out;
}

float shutter_fraction(float shutter_s, float frame_dt_s) {
    if (!(frame_dt_s > 0.0F) || !std::isfinite(shutter_s) || !(shutter_s > 0.0F)) return 0.0F;
    return std::min(shutter_s / frame_dt_s, 1.0F);
}

} // namespace engine::renderer
