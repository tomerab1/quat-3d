#pragma once

// Climate — what the weather is like over the year (ROADMAP R25.1).
//
// A `Climate` holds data curves over the season's year phase (0 = the local
// spring equinox, 0.25 the summer solstice, 0.5 the autumn equinox, 0.75 the
// winter solstice; see engine/scene/season.hpp): the daily mean temperature,
// the diurnal temperature range, the chance that a weather period brings
// precipitation, the cloud cover of dry periods and the mean wind. Curves are
// evenly spaced keys (key i at phase i / n), interpolated with a periodic
// Catmull-Rom spline, so a climate is plain data that games author per world
// (data/seasons/climate.json in the cozy builder).
//
// From a climate and a seed, a deterministic *weather sequence* picks one
// weather kind per period of `weather_period_days` season days (clear, fair,
// overcast, showers, steady rain or snow, storm) with a temperature anomaly,
// and eases from one period into the next. The result drives WeatherState's
// wind, clouds, precipitation and temperature; R13.1's step_weather then
// accumulates wetness and snow from them, so winter snow arrives and melts
// through the existing accumulation. The same kinds are the season-aware
// weather presets: "showers" in January at -3 C fall as snow.
//
// Models (cited where they are implemented, climate.cpp):
//   - rain / snow split: Dai (2008), "Temperature and pressure dependence of
//     the rain-snow phase transition over land and ocean", GRL 35, L12802;
//     the land fit of snow frequency against surface air temperature;
//   - diurnal temperature: the sinusoidal day of Parton & Logan (1981), "A
//     model for diurnal variation in soil and air temperature", Agricultural
//     Meteorology 23, reduced to one harmonic with the maximum ~3 h after
//     solar noon, damped by cloud cover;
//   - the default curves: first harmonics of the annual cycle with the
//     ~1-month thermal lag behind the solstices (Trenberth 1983, "What are
//     the seasons?", BAMS 64), shaped as a cool-temperate inland northern-
//     European climate (stylised, not a station record): mid-winter mean
//     about -1.5 C so snow is likely from the middle of winter, spring
//     showers, a windier and cloudier winter half.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/weather/weather_state.hpp"

namespace engine::weather {

inline constexpr double      k_tropical_year_days = 365.24219; // mean tropical year
inline constexpr std::size_t k_climate_max_keys = 48;

struct Climate {
    // Daily mean air temperature (C).
    std::vector<float> temperature_c = {2.7F, 7.6F,  12.7F, 16.7F, 18.5F, 17.6F,
                                        14.3F, 9.4F, 4.3F,  0.3F,  -1.5F, -0.6F};
    // Daily maximum minus minimum (C), before cloud damping.
    std::vector<float> diurnal_range_c = {6.4F, 7.7F, 8.8F, 9.4F, 9.4F, 8.8F,
                                          7.6F, 6.3F, 5.2F, 4.6F, 4.6F, 5.3F};
    // Chance that a weather period has precipitation (0..1).
    std::vector<float> precipitation_chance = {0.45F, 0.48F, 0.49F, 0.48F, 0.48F, 0.46F,
                                               0.44F, 0.41F, 0.38F, 0.36F, 0.36F, 0.38F};
    // Mean cloud cover of dry periods (0..1).
    std::vector<float> cloud_cover = {0.66F, 0.59F, 0.53F, 0.49F, 0.48F, 0.52F,
                                      0.58F, 0.65F, 0.71F, 0.75F, 0.76F, 0.72F};
    // Mean wind speed 10 m above the ground (m/s).
    std::vector<float> wind_speed = {5.1F, 4.6F, 4.1F, 3.7F, 3.5F, 3.6F,
                                     3.9F, 4.4F, 4.9F, 5.3F, 5.5F, 5.4F};
    // Standard deviation of a period's temperature anomaly (C).
    float temperature_variability_c = 3.0F;
    // Length of one weather period, in days of the season year (synoptic
    // systems last a few days; a cozy 40-minute year may want fewer, longer
    // periods than its 365 days of date suggest).
    float weather_period_days = 3.0F;
    // Fraction of each period spent easing into the next one (0 = switch).
    float transition = 0.3F;

    friend bool operator==(const Climate&, const Climate&) = default;
};

// ---- Curves -------------------------------------------------------------------

enum class ClimateCurve : std::uint8_t {
    temperature_c,
    diurnal_range_c,
    precipitation_chance,
    cloud_cover,
    wind_speed,
};
inline constexpr std::size_t k_climate_curve_count = 5;

struct ClimateCurveDesc {
    ClimateCurve     curve;
    std::string_view name; // JSON key
    float            min;
    float            max;
    std::string_view unit;
    std::string_view hint;
};
[[nodiscard]] std::span<const ClimateCurveDesc> climate_curves();
[[nodiscard]] std::vector<float>&       curve_keys(Climate& climate, ClimateCurve curve);
[[nodiscard]] const std::vector<float>& curve_keys(const Climate& climate, ClimateCurve curve);

// The periodic Catmull-Rom interpolation of `keys` (key i at phase i / n) at
// `phase` (any real; wrapped into [0, 1)). Empty keys give 0.
[[nodiscard]] float sample_curve(std::span<const float> keys, double phase);

// The climate with every curve key and scalar clamped into its range and
// non-finite values replaced by the defaults; empty or over-long curves take
// the default curve.
[[nodiscard]] Climate sanitized(const Climate& climate);

// ---- Sampling -------------------------------------------------------------------

struct ClimateSample {
    float mean_temperature_c = 0.0F;
    float diurnal_range_c = 0.0F;
    // cos(2 pi (solar hour - k_diurnal_max_hour) / 24) in [-1, 1]; 0 when the
    // time of day is unknown (the daily mean is used).
    float diurnal = 0.0F;
    float precipitation_chance = 0.0F;
    float cloud_cover = 0.0F;
    float wind_speed = 0.0F;
};

inline constexpr double k_diurnal_max_hour = 15.0; // local solar time of the daily maximum

// The climate at `phase`; `solar_hour` is the local mean solar time (0..24).
[[nodiscard]] ClimateSample sample_climate(const Climate& climate, double phase,
                                           std::optional<double> solar_hour);

// Air temperature: mean + anomaly + the diurnal swing, halved under full
// cloud (clouds narrow the day / night range).
[[nodiscard]] float air_temperature(const ClimateSample& sample, float anomaly_c, float cloud_cover);

// Fraction of precipitation falling as snow at air temperature `t_c`
// (Dai 2008, land; the fit's ~1 % / ~97.5 % asymptotes normalised to 0 / 1):
// 0.5 at +1.17 C, > 0.95 below -2 C, < 0.02 above +6 C.
[[nodiscard]] float snow_fraction(float t_c);

// ---- Weather kinds, presets and the daily sequence --------------------------------

enum class WeatherKind : std::uint8_t { clear, fair, overcast, showers, rain, storm, snow };
inline constexpr std::size_t k_weather_kind_count = 7;

[[nodiscard]] std::string_view weather_kind_name(WeatherKind kind);
[[nodiscard]] bool parse_weather_kind(std::string_view name, WeatherKind& out);
[[nodiscard]] std::span<const std::string_view> weather_kind_names();

// One weather period of the sequence.
struct WeatherDay {
    WeatherKind       kind = WeatherKind::fair;
    PrecipitationType type = PrecipitationType::rain; // what falls, when anything does
    float             temperature_anomaly_c = 0.0F;

    friend bool operator==(const WeatherDay&, const WeatherDay&) = default;
};

// The period's weather: a pure function of (climate, seed, period), the same
// on every platform. `period` counts weather periods since season day 0
// (see season_days in engine/scene/season.hpp).
[[nodiscard]] WeatherDay weather_day(const Climate& climate, std::uint64_t seed, std::int64_t period);

// The WeatherState for `day` under `sample`: wind, clouds, precipitation and
// temperature from the kind and the climate; wind direction and the surface
// fields (wetness, snow, accumulate) are kept from `base`.
[[nodiscard]] WeatherState weather_for_day(const WeatherDay& day, const ClimateSample& sample,
                                           const WeatherState& base);

// A season-aware preset: `kind` in this climate now. Showers and storms fall
// as snow when it is cold enough (Dai's 50 % point); `rain` and `snow` force
// their type and move the temperature to one where it can fall (>= +2 C for
// rain, <= -1 C for snow).
[[nodiscard]] WeatherState weather_preset(WeatherKind kind, const ClimateSample& sample,
                                          const WeatherState& base);

// The automatic weather at `season_days` (days since season day 0; the period
// is floor(season_days / weather_period_days)), eased into the next period
// over the last `transition` of each one.
struct AutoWeather {
    WeatherState state;
    WeatherDay   day;        // the current period's weather
    std::int64_t period = 0;
    float        blend = 0.0F; // 0 = the current period, toward 1 = the next one
};
[[nodiscard]] AutoWeather auto_weather(const Climate& climate, std::uint64_t seed, double season_days,
                                       const ClimateSample& sample, const WeatherState& base);

} // namespace engine::weather
