#pragma once

// The weather a frame renders with (ROADMAP R13.1): what the renderer reads
// from the scene each frame.

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

namespace engine::renderer {
struct CloudSettings;
}
namespace engine::weather {
struct WeatherState;
}

namespace engine::scene {

// The scene's weather (returns true), or (false) the default weather with the
// wind heading taken from the first Grass field's wind_direction, so scenes
// without weather keep their grass blowing the way they were authored.
[[nodiscard]] bool frame_weather(const entt::registry& registry, weather::WeatherState& out);

// The wind the cloud layer drifts with (world XZ, m/s): the weather's mean
// wind (the large-scale flow — gusts are boundary-layer turbulence) scaled by
// the logarithmic profile to the middle of the cloud layer.
[[nodiscard]] glm::vec2 cloud_wind(const weather::WeatherState& state,
                                   const renderer::CloudSettings& clouds);

} // namespace engine::scene
