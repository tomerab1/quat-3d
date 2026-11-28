#pragma once

// WeatherState — the scene's weather (ROADMAP R13.1).
//
// One POD, carried as a component by a single entity (the scene's weather
// singleton: find_weather() returns it, the codec refuses a second one). It
// drives the global wind field (wind_field.hpp: grass on the GPU, physics and
// audio through the CPU reference), the cloud layer's cover and drift, and the
// wet / snowy surface layers of the deferred lighting pass.
//
// Like the renderer's settings (A8), every field is listed in a descriptor
// table (weather_fields()): the component codec, the remote weather.* methods,
// the quat-mcp schema and the editor widgets all follow from one entry, with
// one range and one default.
//
// Conventions: wind_dir_deg is the heading the wind blows TOWARD, measured in
// the XZ plane from +X toward +Z (the clouds' wind_dir_deg convention); wind
// speed is the mean at the 10 m reference height.

#include <cstdint>
#include <span>
#include <string_view>

#include <entt/entity/fwd.hpp>

namespace engine::weather {

enum class PrecipitationType : std::uint8_t { rain, snow };

struct WeatherState {
    // ---- wind ----
    float wind_dir_deg = 35.0F; // heading the wind blows toward (+X toward +Z)
    float wind_speed = 4.0F;    // mean m/s at 10 m above the ground
    float gustiness = 0.35F;    // 0 = steady, 1 = strong gust fronts
    float turbulence = 0.3F;    // 0 = laminar, 1 = strongly swirling
    // ---- sky ----
    float cloud_cover = 0.42F;  // fraction of the sky covered (0..1)
    // ---- precipitation ----
    float             precipitation = 0.0F; // 0 = dry, 1 = downpour / blizzard
    PrecipitationType precipitation_type = PrecipitationType::rain;
    // ---- surfaces ----
    float wetness = 0.0F;        // 0 = dry, 1 = soaked (puddles from ~0.55)
    float snow = 0.0F;           // snow accumulation (0 = none, 1 = deep cover)
    float temperature_c = 12.0F; // air temperature (snow melts above 0 C)
    // Evolve wetness and snow from precipitation and temperature as simulated
    // time passes (step_weather); off = they stay where they are set.
    bool accumulate = true;
};

// ---- Descriptor table (A8 style) ---------------------------------------------

enum class WeatherFieldType : std::uint8_t { number, boolean, precipitation_type };

// Where a field lives in a WeatherState; exactly one pointer is set.
struct WeatherFieldRef {
    float*             f = nullptr;
    bool*              b = nullptr;
    PrecipitationType* p = nullptr;
};

struct WeatherFieldDesc {
    std::string_view group; // "wind", "sky", "precipitation", "surfaces"
    std::string_view name;  // JSON key
    std::string_view label; // editor widget label
    WeatherFieldType type;
    float            min = 0.0F; // inclusive range (numbers)
    float            max = 1.0F;
    float            default_value = 0.0F; // equals the struct default
    std::string_view unit; // "" when unitless
    std::string_view hint; // one line (tooltip / schema)
    WeatherFieldRef (*ref)(WeatherState&) = nullptr;
};

// Every field in display order.
[[nodiscard]] std::span<const WeatherFieldDesc> weather_fields();
[[nodiscard]] const WeatherFieldDesc* find_weather_field(std::string_view name);

// Names of the precipitation types, as the codec spells them.
[[nodiscard]] std::string_view precipitation_type_name(PrecipitationType type);
[[nodiscard]] bool parse_precipitation_type(std::string_view name, PrecipitationType& out);

// Numbers clamped into their table ranges; non-finite values take the default.
[[nodiscard]] WeatherState sanitized(const WeatherState& state);

// ---- Accumulation ------------------------------------------------------------

// Time constants of step_weather (seconds), exposed for tests.
inline constexpr float k_wetting_time_s = 90.0F;   // rain soaking dry ground (full intensity)
inline constexpr float k_drying_time_s = 1800.0F;  // evaporation at 0 C, calm air
inline constexpr float k_snowfall_time_s = 300.0F; // blizzard covering bare ground
inline constexpr float k_melt_time_s = 900.0F;     // melting at +10 C
inline constexpr float k_snow_max_temperature_c = 1.0F; // snow settles up to this

// Advances wetness and snow by `dt` seconds of simulated time (no-op when
// `accumulate` is off or dt <= 0):
//   rain wets the surfaces toward 1 (faster when heavier);
//   snow settles while the air is at or below k_snow_max_temperature_c;
//   above 0 C snow melts (rate proportional to the temperature) and its
//   meltwater wets the surfaces;
//   without rain the surfaces dry, faster when warm and windy.
// Deterministic: the same inputs give the same state on every platform.
void step_weather(WeatherState& state, float dt);

// ---- ECS ----------------------------------------------------------------------

// The scene's weather entity (the first entity carrying a WeatherState), or
// entt::null.
[[nodiscard]] entt::entity find_weather(const entt::registry& registry);

// Runs step_weather on the scene's weather (sim time; the frame loop calls it
// with the simulation dt, so pausing freezes the weather too).
void weather_system(entt::registry& registry, float dt);

} // namespace engine::weather
