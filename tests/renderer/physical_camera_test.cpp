// R20 physical camera: EV100 and the exposure it gives (Lagarde & de
// Rousiers 2014), focal length <-> field of view, the thin-lens circle of
// confusion against closed forms, and the shutter fraction motion blur uses.

#include <cmath>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/physical_camera.hpp"

namespace {

namespace r = engine::renderer;

TEST(PhysicalCamera, Ev100FollowsTheExposureTriangle) {
    // Sunny 16: f/16, 1/100 s, ISO 100 -> log2(256 * 100) = 14.64.
    EXPECT_NEAR(r::ev100(16.0F, 1.0F / 100.0F, 100.0F), std::log2(25600.0F), 1e-4F);
    // f/1, 1 s, ISO 100 is EV 0 by definition.
    EXPECT_NEAR(r::ev100(1.0F, 1.0F, 100.0F), 0.0F, 1e-6F);
    // One stop each: opening the aperture by sqrt(2), doubling the time or the ISO.
    const float base = r::ev100(4.0F, 1.0F / 60.0F, 200.0F);
    EXPECT_NEAR(r::ev100(4.0F / std::sqrt(2.0F), 1.0F / 60.0F, 200.0F), base - 1.0F, 1e-4F);
    EXPECT_NEAR(r::ev100(4.0F, 2.0F / 60.0F, 200.0F), base - 1.0F, 1e-4F);
    EXPECT_NEAR(r::ev100(4.0F, 1.0F / 60.0F, 400.0F), base - 1.0F, 1e-4F);
    // Exposure maps a luminance of 1.2 * 2^EV100 to 1.
    for (const float ev : {-2.0F, 0.0F, 9.0F, 15.0F}) {
        EXPECT_NEAR(r::exposure_from_ev100(ev) * 1.2F * std::exp2(ev), 1.0F, 1e-5F) << ev;
    }
    // The struct overload sanitises: an absurd shutter is clamped, not NaN.
    r::PhysicalCamera c;
    c.shutter_s = -1.0F;
    EXPECT_TRUE(std::isfinite(r::ev100(c)));
}

TEST(PhysicalCamera, FocalLengthAndFieldOfView) {
    // A 50 mm lens on a full-frame (24 mm tall) sensor: 2 atan(12 / 50) = 26.99 deg.
    const float fov = r::fov_from_focal_length(50.0F, 24.0F);
    EXPECT_NEAR(glm::degrees(fov), 26.9915F, 1e-3F);
    EXPECT_NEAR(r::focal_length_from_fov(fov, 24.0F), 50.0F, 1e-3F);
    r::PhysicalCamera c;
    // Focal length 0: the camera keeps its FOV and reports the matching lens.
    EXPECT_FLOAT_EQ(r::effective_fov_y(c, glm::radians(60.0F)), glm::radians(60.0F));
    EXPECT_NEAR(r::effective_focal_length_mm(c, glm::radians(60.0F)), 12.0F / std::tan(glm::radians(30.0F)),
                1e-3F);
    // A focal length replaces the FOV.
    c.focal_length_mm = 50.0F;
    EXPECT_NEAR(r::effective_fov_y(c, glm::radians(60.0F)), fov, 1e-6F);
    EXPECT_FLOAT_EQ(r::effective_focal_length_mm(c, glm::radians(60.0F)), 50.0F);
}

// sanitized() keeps a positive focal length at or above min_focal_mm, so the
// derived FOV stays well below 180 degrees (0 still means "use the FOV").
TEST(PhysicalCamera, TinyFocalLengthsAreClampedToTheMinimum) {
    using L = r::PhysicalCameraLimits;
    r::PhysicalCamera c;
    c.focal_length_mm = 0.001F;
    EXPECT_FLOAT_EQ(r::sanitized(c).focal_length_mm, L::min_focal_mm);
    EXPECT_LT(r::effective_fov_y(c, glm::radians(60.0F)), 3.1F);
    c.sensor_height_mm = L::max_sensor_mm; // widest sensor on the shortest lens
    EXPECT_LE(r::effective_fov_y(c, glm::radians(60.0F)), 3.1F);
    c.sensor_height_mm = 24.0F;
    c.focal_length_mm = -5.0F;
    EXPECT_FLOAT_EQ(r::sanitized(c).focal_length_mm, 0.0F);
    EXPECT_FLOAT_EQ(r::effective_fov_y(c, glm::radians(60.0F)), glm::radians(60.0F));
}

TEST(PhysicalCamera, ThinLensCircleOfConfusion) {
    // 50 mm f/2 focused at 2 m; a point at 4 m:
    // c = f^2 / (N (S - f)) * (z - S) / z = 2500 / 3900 * 0.5 = 0.3205 mm.
    EXPECT_NEAR(r::coc_diameter_mm(4.0F, 50.0F, 2.0F, 2.0F), 2500.0F / 3900.0F * 0.5F, 1e-5F);
    EXPECT_NEAR(r::coc_diameter_mm(2.0F, 50.0F, 2.0F, 2.0F), 0.0F, 1e-6F); // in focus
    EXPECT_LT(r::coc_diameter_mm(1.0F, 50.0F, 2.0F, 2.0F), 0.0F);           // near: negative
    // Tends to f^2 / (N (S - f)) at infinity; stopping down shrinks it.
    EXPECT_NEAR(r::coc_diameter_mm(1e6F, 50.0F, 2.0F, 2.0F), 2500.0F / 3900.0F, 1e-3F);
    EXPECT_NEAR(r::coc_diameter_mm(4.0F, 50.0F, 4.0F, 2.0F),
                r::coc_diameter_mm(4.0F, 50.0F, 2.0F, 2.0F) * 0.5F, 1e-6F);

    // The per-pixel form the passes evaluate: radius_px = scale + bias / z.
    r::PhysicalCamera c;
    c.focal_length_mm = 50.0F;
    c.aperture_f = 2.0F;
    c.focus_distance_m = 2.0F;
    const float height_px = 1080.0F;
    const r::CocParams p = r::coc_params(c, glm::radians(60.0F), height_px);
    for (const float z : {0.5F, 1.0F, 2.0F, 4.0F, 100.0F}) {
        const float expected = 0.5F * r::coc_diameter_mm(z, 50.0F, 2.0F, 2.0F) / 24.0F * height_px;
        EXPECT_NEAR(p.radius_px(z), expected, 1e-3F * std::max(1.0F, std::abs(expected))) << z;
    }
    EXPECT_NEAR(p.radius_px(2.0F), 0.0F, 1e-4F);
    // A focus closer than the focal length is clamped, never divides by zero.
    c.focus_distance_m = 0.01F;
    const r::CocParams q = r::coc_params(c, glm::radians(60.0F), height_px);
    EXPECT_TRUE(std::isfinite(q.scale) && std::isfinite(q.bias));
}

TEST(PhysicalCamera, ShutterFractionIsTheShutterAngle) {
    // A 180 degree shutter at 24 fps is 1/48 s.
    EXPECT_NEAR(r::shutter_fraction(1.0F / 48.0F, 1.0F / 24.0F), 0.5F, 1e-6F);
    // Open longer than a frame: capped at 360 degrees.
    EXPECT_FLOAT_EQ(r::shutter_fraction(1.0F / 30.0F, 1.0F / 60.0F), 1.0F);
    EXPECT_FLOAT_EQ(r::shutter_fraction(1.0F / 50.0F, 0.0F), 0.0F);
    EXPECT_FLOAT_EQ(r::shutter_fraction(NAN, 1.0F / 60.0F), 0.0F);
}

} // namespace
