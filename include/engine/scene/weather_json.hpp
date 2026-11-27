#pragma once

// JSON codec of weather::WeatherState (ROADMAP R13.1), driven by the field
// table in engine/weather/weather_state.hpp. Shared by the component codec
// ("weather" component: scene files, scene.*) and the remote weather.*
// methods.
//
// Shape: a flat object {"wind_speed": 6, "precipitation_type": "snow", ...}
// — numbers, booleans, and the precipitation type as "rain" | "snow".

#include <expected>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::weather {
struct WeatherState;
}

namespace engine::scene {

[[nodiscard]] nlohmann::json encode_weather(const weather::WeatherState& state);

// Merges `fields` into `state`; absent fields keep their values and nothing
// changes on error.
//   strict (remote requests): unknown fields, wrong types and values outside
//     a field's range are errors naming the field and what it takes.
//   lenient (scene files): unknown fields are ignored and numbers outside the
//     range are clamped; wrong types are still errors.
// `context` prefixes error messages (e.g. "weather").
[[nodiscard]] std::expected<void, core::Error>
apply_weather_json(weather::WeatherState& state, const nlohmann::json& fields, bool strict,
                   std::string_view context = "weather");

// The field table: {"fields": [{name, group, label, type: "number" |
// "boolean" | "enum", min, max, default, unit, hint, values}]}.
[[nodiscard]] nlohmann::json weather_schema();

} // namespace engine::scene
