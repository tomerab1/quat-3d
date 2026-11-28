#pragma once

// The weather a frame renders with (ROADMAP R13.1): what the renderer reads
// from the scene each frame.

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/renderer/cloud_settings.hpp"

namespace engine::weather {
struct WeatherState;
}

namespace engine::scene {

// The scene's weather (returns true), or (false) the default weather with the
// wind heading taken from the first Grass field's wind_direction, so scenes
// without weather keep their grass blowing the way they were authored.
[[nodiscard]] bool frame_weather(const entt::registry& registry, weather::WeatherState& out);

// The wind the cloud layer drifts with (world XZ, m/s): the weather's mean
// wind (the large-scale flow — gusts are boundary-layer turbulence) times
// weather::k_geostrophic_factor. Clouds sit above the surface layer, where
// the log profile does not apply; the fixed factor is the usual ratio of the
// free-atmosphere wind to the 10 m wind over land (veering is not modelled).
[[nodiscard]] glm::vec2 cloud_wind(const weather::WeatherState& state,
                                   const renderer::CloudSettings& clouds);

// The cloud layer a frame renders (sky, IBL, cloud shadow map) from the
// renderer's settings.
//   Without weather (`weather` null): the settings exactly as before R13.1 —
//     drift = wind_speed x `time_s` along wind_dir_deg, camera-relative
//     (offset_km 0).
//   With weather: its cloud cover and precipitation; drift is the
//     integrated `drift_km` of its wind; anchored to the world (offset_km =
//     camera XZ in km - drift: the field coordinate of world point X is
//     X - drift, so features move downwind and stay put as the camera
//     moves).
[[nodiscard]] renderer::CloudSettings frame_clouds(const renderer::CloudSettings& settings,
                                                   float time_s,
                                                   const weather::WeatherState* weather,
                                                   glm::vec2 drift_km, const glm::vec3& camera);

} // namespace engine::scene
