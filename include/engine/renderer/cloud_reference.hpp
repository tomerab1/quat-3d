#pragma once

// The volumetric cloud layer's GPU constants and the CPU reference of its
// math (ROADMAP R13.4; lib/clouds.slang).
//
// CloudLayerGpu is the one block every cloud shader reads (the march, the
// cloud shadow map). make_cloud_layer() builds it from the frame's
// CloudSettings (after scene::frame_clouds applied the weather) and its
// lights. The functions below mirror lib/clouds.slang operation for
// operation so tests can check the shaders against them: the
// energy-conserving step integration (Hillaire 2015), Wrenninge's
// multiple-scattering octaves, the dual-lobe phase function, the
// Nubis density (reading the CPU-baked noise of cloud_noise.hpp with the
// GPU's filtering) and the cloud shadow transmittance.

#include <cstdint>

#include <glm/glm.hpp>

#include "engine/renderer/cloud_noise.hpp"
#include "engine/renderer/cloud_settings.hpp"

namespace engine::renderer {

// Mirrors CloudLayer in lib/clouds.slang (std140, 160 bytes).
struct CloudLayerGpu {
    glm::vec4 shell{0.0F};         // base km, thickness km, coverage, type
    glm::vec4 medium{0.0F};        // extinction /km, albedo, detail erosion, precipitation
    glm::vec4 scale{0.0F};         // 1/base tile km, 1/detail tile km, 1/weather tile km, curl km
    glm::vec4 offset{0.0F};        // base offset km (xy), detail offset km (zw)
    glm::vec4 cirrus{0.0F};        // coverage, altitude km, optical depth, 1/tile km
    glm::vec4 cirrus_offset{0.0F}; // offset km (xy), detail drift km (z)
    glm::vec4 octaves{0.0F};       // a, b, c, count
    glm::vec4 phase{0.0F};         // forward g, backward g, backward weight, ambient scale
    glm::vec4 sun{0.0F};           // toward the sun, extra-atmospheric irradiance (rel.)
    glm::vec4 moon{0.0F};          // toward the moon, irradiance (rel., 0 = none)
};
static_assert(sizeof(CloudLayerGpu) == 160, "must match CloudLayer in lib/clouds.slang");

// Multiple-scattering octaves (Wrenninge et al. 2013; Hillaire 2016 uses the
// same values): energy-conserving because a <= b.
inline constexpr float k_cloud_octave_a = 0.5F;
inline constexpr float k_cloud_octave_b = 0.5F;
inline constexpr float k_cloud_octave_c = 0.5F;
inline constexpr int   k_cloud_octaves = 3;
// Single-scattering albedo of cloud droplets in the visible (absorption is
// negligible; Hillaire 2016).
inline constexpr float k_cloud_albedo = 0.99F;

struct CloudLights {
    glm::vec3 sun_to{0.0F, 1.0F, 0.0F}; // unit, toward the sun
    float     sun_irradiance = 1.0F;     // extra-atmospheric, relative to the reference sun
    glm::vec3 moon_to{0.0F, -1.0F, 0.0F};
    float     moon_irradiance = 0.0F;    // 0 = no moon
};

// The frame's layer. `clouds` is the frame's CloudSettings (frame_clouds);
// `world_anchored` is true when its offset_km already anchors the field to
// the world (scenes with weather); otherwise the drift is wind_speed x
// time_s along wind_dir_deg and the camera's position anchors it here.
[[nodiscard]] CloudLayerGpu make_cloud_layer(const CloudSettings& clouds, bool world_anchored,
                                             const glm::vec3& camera, const CloudLights& lights);

namespace cloud_reference {

[[nodiscard]] float hg_phase(float cos_theta, float g);
[[nodiscard]] float cloud_phase(const CloudLayerGpu& l, float cos_theta, float c_pow);
[[nodiscard]] float height_profile(float h, float type);

// Hillaire 2015's step integration: returns trans x (S - S e^{-sigma dt}) /
// sigma and multiplies `trans` by e^{-sigma dt}.
[[nodiscard]] glm::vec3 integrate_step(const glm::vec3& s, float sigma_t, float dt, float& trans);

// The octave sum for one light (per unit length, before integration).
[[nodiscard]] glm::vec3 light_scattering(const CloudLayerGpu& l, const glm::vec3& irradiance,
                                         float sigma_s, float tau, float cos_theta);

// Density at camera-relative p (xz km, y altitude km) from CPU-baked noise.
[[nodiscard]] float density(const CloudLayerGpu& l, const cloud_noise::Volumes& noise,
                            const glm::vec3& p, bool cheap);

// lib/clouds.slang cloud_shadow_transmittance.
[[nodiscard]] float shadow_transmittance(const CloudLayerGpu& l, const cloud_noise::Volumes& noise,
                                         const glm::vec3& offset_km, const glm::vec3& sun_dir,
                                         int steps);

} // namespace cloud_reference

} // namespace engine::renderer
