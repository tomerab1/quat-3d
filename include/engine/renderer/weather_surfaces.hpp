#pragma once

// What the lighting pass needs from the weather (ROADMAP R13.1): how wet and
// how snowy surfaces are, and where the snowline sits. lighting.slang feeds it
// to the weather surface layers (lib/weather_surface.slang).

#include <algorithm>
#include <cmath>

#include "engine/weather/weather_state.hpp"

namespace engine::renderer {

// Snowline used when the scene has no terrain: weather snow then depends on
// the accumulated amount alone.
inline constexpr float k_no_snowline_m = 1.0e6F;

struct WeatherSurfaces {
    float wetness = 0.0F;               // 0..1
    float snow = 0.0F;                  // 0..1 accumulated snow
    float snowline_m = k_no_snowline_m; // the terrain's snowline (world height)

    [[nodiscard]] static WeatherSurfaces from(const weather::WeatherState& state,
                                              float snowline_m = k_no_snowline_m) {
        const weather::WeatherState s = weather::sanitized(state);
        return {s.wetness, s.snow, snowline_m};
    }

    // Water level of the puddle height field (0..1): puddles start once the
    // surfaces are soaked (Lagarde's accumulation follows wetting) and cover
    // a little under half of flat ground when flooded.
    [[nodiscard]] float puddle_level() const {
        const float t = std::clamp((wetness - 0.55F) / 0.45F, 0.0F, 1.0F);
        return 0.45F * t * t * (3.0F - 2.0F * t);
    }

    [[nodiscard]] WeatherSurfaces sanitized() const {
        const auto unit = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0F, 1.0F) : 0.0F; };
        return {unit(wetness), unit(snow),
                std::isfinite(snowline_m) ? snowline_m : k_no_snowline_m};
    }
};

} // namespace engine::renderer
