#include "engine/weather/weather_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

#include <entt/entity/registry.hpp>

namespace engine::weather {

namespace {

template <auto Field>
WeatherFieldRef ref(WeatherState& s) {
    auto& v = s.*Field;
    using V = std::remove_cvref_t<decltype(v)>;
    if constexpr (std::is_same_v<V, bool>) {
        return {.b = &v};
    } else if constexpr (std::is_same_v<V, PrecipitationType>) {
        return {.p = &v};
    } else {
        static_assert(std::is_same_v<V, float>, "weather fields are float, bool or enum");
        return {.f = &v};
    }
}

using T = WeatherFieldType;
using W = WeatherState;

constexpr std::array k_fields = {
    // ---- wind ----
    WeatherFieldDesc{"wind", "wind_dir_deg", "wind direction", T::number, -180.0F, 180.0F, 35.0F,
                     "deg", "Heading the wind blows toward (from +X toward +Z)",
                     &ref<&W::wind_dir_deg>},
    WeatherFieldDesc{"wind", "wind_speed", "wind speed", T::number, 0.0F, 60.0F, 4.0F, "m/s",
                     "Mean wind speed 10 m above the ground", &ref<&W::wind_speed>},
    WeatherFieldDesc{"wind", "gustiness", "gustiness", T::number, 0.0F, 1.0F, 0.35F, "",
                     "Strength of the gust fronts travelling downwind", &ref<&W::gustiness>},
    WeatherFieldDesc{"wind", "turbulence", "turbulence", T::number, 0.0F, 1.0F, 0.3F, "",
                     "Small-scale swirl in direction and speed", &ref<&W::turbulence>},
    // ---- sky ----
    WeatherFieldDesc{"sky", "cloud_cover", "cloud cover", T::number, 0.0F, 1.0F, 0.42F, "",
                     "Fraction of the sky covered by clouds (overrides the clouds setting)",
                     &ref<&W::cloud_cover>},
    // ---- precipitation ----
    WeatherFieldDesc{"precipitation", "precipitation", "intensity", T::number, 0.0F, 1.0F, 0.0F,
                     "", "0 = dry, 1 = downpour or blizzard", &ref<&W::precipitation>},
    WeatherFieldDesc{"precipitation", "precipitation_type", "type", T::precipitation_type, 0.0F,
                     1.0F, 0.0F, "", "What falls: rain or snow", &ref<&W::precipitation_type>},
    // ---- surfaces ----
    WeatherFieldDesc{"surfaces", "wetness", "wetness", T::number, 0.0F, 1.0F, 0.0F, "",
                     "Surface wetness: darker, glossier; puddles on flat ground above ~0.55",
                     &ref<&W::wetness>},
    WeatherFieldDesc{"surfaces", "snow", "snow", T::number, 0.0F, 1.0F, 0.0F, "",
                     "Snow accumulated on upward-facing surfaces", &ref<&W::snow>},
    WeatherFieldDesc{"surfaces", "temperature_c", "temperature", T::number, -40.0F, 45.0F, 12.0F,
                     "C", "Air temperature: snow settles at or below 1 C and melts above 0 C",
                     &ref<&W::temperature_c>},
    WeatherFieldDesc{"surfaces", "accumulate", "accumulate", T::boolean, 0.0F, 1.0F, 1.0F, "",
                     "Let rain, snowfall, melt and drying change wetness and snow over time",
                     &ref<&W::accumulate>},
};

constexpr std::array<std::string_view, 2> k_precipitation_names = {"rain", "snow"};

// Exponential approach of `v` toward `target` with time constant `tau` over
// `dt` (exact for constant rates, so the result does not depend on the frame
// rate).
[[nodiscard]] float approach(float v, float target, float dt, float tau) {
    if (!(tau > 0.0F)) return target;
    return target + (v - target) * std::exp(-dt / tau);
}

} // namespace

std::span<const WeatherFieldDesc> weather_fields() { return k_fields; }

const WeatherFieldDesc* find_weather_field(std::string_view name) {
    for (const WeatherFieldDesc& d : k_fields) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

std::string_view precipitation_type_name(PrecipitationType type) {
    const auto i = static_cast<std::size_t>(type);
    return i < k_precipitation_names.size() ? k_precipitation_names[i] : "rain";
}

bool parse_precipitation_type(std::string_view name, PrecipitationType& out) {
    for (std::size_t i = 0; i < k_precipitation_names.size(); ++i) {
        if (k_precipitation_names[i] == name) {
            out = static_cast<PrecipitationType>(i);
            return true;
        }
    }
    return false;
}

WeatherState sanitized(const WeatherState& state) {
    WeatherState s = state;
    for (const WeatherFieldDesc& d : k_fields) {
        const WeatherFieldRef r = d.ref(s);
        if (r.f != nullptr) {
            *r.f = std::isfinite(*r.f) ? std::clamp(*r.f, d.min, d.max) : d.default_value;
        } else if (r.p != nullptr && static_cast<std::size_t>(*r.p) >= k_precipitation_names.size()) {
            *r.p = PrecipitationType::rain;
        }
    }
    return s;
}

void step_weather(WeatherState& state, float dt) {
    if (!state.accumulate || !(dt > 0.0F)) return;
    WeatherState s = sanitized(state);
    const float t = s.temperature_c;
    const bool raining = s.precipitation > 0.0F && s.precipitation_type == PrecipitationType::rain;
    const bool snowing = s.precipitation > 0.0F && s.precipitation_type == PrecipitationType::snow;

    // Snow: settles while cold enough, melts above freezing.
    float melt_rate = 0.0F; // fraction of the cover melting per second
    if (snowing && t <= k_snow_max_temperature_c) {
        s.snow = approach(s.snow, 1.0F, dt, k_snowfall_time_s / s.precipitation);
    }
    if (t > 0.0F && s.snow > 0.0F) {
        const float tau = k_melt_time_s * 10.0F / t;
        const float before = s.snow;
        s.snow = approach(s.snow, 0.0F, dt, tau);
        melt_rate = (before - s.snow) / dt;
    }

    // Wetness: rain and meltwater soak the surfaces; otherwise they dry,
    // faster when warm and windy.
    if (raining) {
        s.wetness = approach(s.wetness, 1.0F, dt, k_wetting_time_s / s.precipitation);
    } else if (melt_rate > 0.0F) {
        // Meltwater soaks at most as fast as moderate rain.
        const float soak = std::min(melt_rate * k_wetting_time_s * 20.0F, 1.0F);
        if (soak > 0.0F) s.wetness = approach(s.wetness, 1.0F, dt, k_wetting_time_s / soak);
    } else {
        const float warmth = 1.0F + std::max(t, 0.0F) / 15.0F;
        const float breeze = 1.0F + s.wind_speed / 8.0F;
        s.wetness = approach(s.wetness, 0.0F, dt, k_drying_time_s / (warmth * breeze));
    }
    state.wetness = std::clamp(s.wetness, 0.0F, 1.0F);
    state.snow = std::clamp(s.snow, 0.0F, 1.0F);
}

entt::entity find_weather(const entt::registry& registry) {
    for (const entt::entity e : registry.view<const WeatherState>()) return e;
    return entt::null;
}

void weather_system(entt::registry& registry, float dt) {
    const entt::entity e = find_weather(registry);
    if (e != entt::null) step_weather(registry.get<WeatherState>(e), dt);
}

} // namespace engine::weather
