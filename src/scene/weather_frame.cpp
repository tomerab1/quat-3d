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
    (void)clouds; // the cloud layer sits in the free atmosphere at any altitude
    const float heading = glm::radians(s.wind_dir_deg);
    const float speed = s.wind_speed * weather::k_geostrophic_factor;
    return glm::vec2(std::cos(heading), std::sin(heading)) * speed;
}

renderer::CloudSettings frame_clouds(const renderer::CloudSettings& settings, float time_s,
                                     const weather::WeatherState* weather, glm::vec2 drift_km,
                                     const glm::vec3& camera) {
    renderer::CloudSettings c = settings;
    if (weather == nullptr) {
        c.time_s = time_s;
        c.offset_km = glm::vec2(0.0F);
        return c;
    }
    const weather::WeatherState w = weather::sanitized(*weather);
    c.coverage = w.cloud_cover;
    c.density *= 1.0F + w.precipitation;
    c.time_s = 0.0F;
    c.offset_km = drift_km + glm::vec2(camera.x, camera.z) * 0.001F;
    return c;
}

} // namespace engine::scene
