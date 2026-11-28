#pragma once

// The sun of an HDRI panorama (ROADMAP R21). An outdoor HDRI holds the sun as
// a few extremely bright texels: image-based lighting alone renders it as
// soft fill without a shadow (and half-float maps cannot even store it).
// The analysis finds it so the environment can be split the way production
// renderers light outdoor scenes: the sun's texels are clamped out of the
// baked maps (to the sky level around them) and the clamped-off energy
// becomes an analytic directional light with cascaded shadows.
//
//   * The brightest texel seeds the search; the sky level is the median
//     luminance of a ring 6-10 degrees around it.
//   * A sun exists when the peak is at least `k_min_contrast` times that
//     level; every texel within `k_cone_deg` brighter than
//     `k_clamp_factor` x the sky level is clamped to it.
//   * The directional light gets the clamped-off irradiance (sum of
//     removed radiance x texel solid angle) and its energy-weighted
//     direction.

#include <glm/glm.hpp>

namespace engine::asset {
struct HdrImage;
}

namespace engine::renderer {

struct HdriSun {
    bool      found = false;
    glm::vec3 direction{0.0F, 1.0F, 0.0F}; // unit, toward the sun, panorama space
    glm::vec3 irradiance{0.0F};             // clamped-off energy: integral of L dw (linear RGB)
    float     cone_cos = 2.0F;              // cos of the removal cone (> 1: nothing removed)
    float     clamp_luminance = 0.0F;       // cone texels are clamped to this luminance
    float     sky_luminance = 0.0F;         // median luminance around the sun

    static constexpr float k_cone_deg = 5.0F;
    static constexpr float k_min_contrast = 20.0F;
    static constexpr float k_clamp_factor = 2.0F;
};

[[nodiscard]] HdriSun analyze_hdri_sun(const asset::HdrImage& image);

// The HDRI's sun as a directional light, in world space after the
// environment's rotation and intensity. `found` false: the panorama has no
// sun (overcast, interior); intensity is then 0.
struct HdriSunLight {
    bool      found = false;
    glm::vec3 direction_to_sun{0.0F, 1.0F, 0.0F};
    glm::vec3 color{1.0F};     // max component 1
    float     intensity = 0.0F; // colour * intensity = the clamped-off irradiance
};

// `sun` placed in the world: the panorama rotated by `rotation_deg` about +Y
// (counter-clockwise seen from above) and scaled by `intensity` (as the
// environment's bake applies them).
[[nodiscard]] HdriSunLight hdri_sun_light(const HdriSun& sun, float rotation_deg, float intensity);

} // namespace engine::renderer
