#include "engine/scene/weather_frame.hpp"

#include <cmath>

#include <entt/entity/registry.hpp>

#include "engine/renderer/cloud_settings.hpp"
#include "engine/scene/components.hpp"
#include "engine/weather/weather_state.hpp"
#include "engine/weather/wind_field.hpp"

namespace engine::scene {

bool frame_weather(const entt::registry& registry, weather::WeatherState& out) {
    if (const entt::entity e = weather::find_weather(registry); e != entt::null) {
        out = weather::sanitized(registry.get<weather::WeatherState>(e));
        return true;
    }
    out = weather::WeatherState{};
    for (const auto [e, grass] : registry.view<const Grass>().each()) {
        const glm::vec2 d = grass.settings.wind_direction;
        if (std::isfinite(d.x) && std::isfinite(d.y) && glm::length(d) > 1e-6F) {
            out.wind_dir_deg = glm::degrees(std::atan2(d.y, d.x));
        }
        break;
    }
    return false;
}

glm::vec2 cloud_wind(const weather::WeatherState& state, const renderer::CloudSettings& clouds) {
    const weather::WeatherState s = weather::sanitized(state);
    const float heading = glm::radians(s.wind_dir_deg);
    const float mid_m = (clouds.bottom_km + 0.5F * clouds.thickness_km) * 1000.0F;
    const float speed = s.wind_speed * weather::wind_profile(mid_m);
    return glm::vec2(std::cos(heading), std::sin(heading)) * speed;
}

} // namespace engine::scene
