// Weather kinds, season-aware presets and the seeded weather sequence —
// see include/engine/weather/climate.hpp.

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/weather/climate.hpp"

namespace engine::weather {

namespace {

constexpr std::array<std::string_view, k_weather_kind_count> k_kind_names = {
    "clear", "fair", "overcast", "showers", "rain", "storm", "snow"};

// What each kind looks like: cloud cover, precipitation intensity, wind as a
// multiple of the climate's mean, gustiness, turbulence.
struct KindLook {
    float cloud;
    float precipitation;
    float wind_factor;
    float gustiness;
    float turbulence;
};
constexpr std::array<KindLook, k_weather_kind_count> k_looks = {{
    {0.08F, 0.0F, 0.7F, 0.2F, 0.2F},     // clear
    {0.38F, 0.0F, 1.0F, 0.3F, 0.3F},     // fair
    {0.9F, 0.0F, 1.1F, 0.35F, 0.3F},     // overcast
    {0.7F, 0.35F, 1.25F, 0.55F, 0.45F},  // showers
    {0.95F, 0.6F, 1.3F, 0.4F, 0.35F},    // rain (steady)
    {1.0F, 0.95F, 2.4F, 0.85F, 0.7F},    // storm
    {0.95F, 0.55F, 0.9F, 0.3F, 0.3F},    // snow (steady)
}};

// Shares of the precipitating kinds: showers, then steady, then storms.
constexpr double k_showers_share = 0.45;
constexpr double k_steady_share = 0.42;
// Spread of a dry period's drawn cloudiness around the climate mean.
constexpr double k_dry_cloud_spread = 0.9;
// Temperatures the forced presets move to so that what they name can fall.
constexpr float k_rain_min_temperature_c = 2.0F;
constexpr float k_snow_preset_max_temperature_c = -1.0F;

[[nodiscard]] const KindLook& look_of(WeatherKind k) { return k_looks[static_cast<std::size_t>(k)]; }

// SplitMix64 (Steele, Lea & Flood, "Fast splittable pseudorandom number
// generators", OOPSLA 2014): a stateless integer hash of (seed, period), so
// every period's weather can be computed on its own, in any order.
struct SplitMix {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    // Uniform in [0, 1) with 53 random bits (exact on every platform).
    double uniform() { return static_cast<double>(next() >> 11U) * 0x1.0p-53; }
};

[[nodiscard]] bool precipitating(WeatherKind k) { return look_of(k).precipitation > 0.0F; }

[[nodiscard]] double wrap01(double x) {
    const double f = x - std::floor(x);
    return f >= 1.0 ? 0.0 : f;
}

[[nodiscard]] float lerp(float a, float b, float t) { return a + (b - a) * t; }

} // namespace

std::string_view weather_kind_name(WeatherKind kind) {
    const auto i = static_cast<std::size_t>(kind);
    return i < k_kind_names.size() ? k_kind_names[i] : "fair";
}

bool parse_weather_kind(std::string_view name, WeatherKind& out) {
    for (std::size_t i = 0; i < k_kind_names.size(); ++i) {
        if (k_kind_names[i] == name) {
            out = static_cast<WeatherKind>(i);
            return true;
        }
    }
    return false;
}

std::span<const std::string_view> weather_kind_names() { return k_kind_names; }

WeatherDay weather_day(const Climate& climate, std::uint64_t seed, std::int64_t period) {
    const double period_days = climate.weather_period_days;
    const double centre = (static_cast<double>(period) + 0.5) * period_days;
    const ClimateSample sample =
        sample_climate(climate, wrap01(centre / k_tropical_year_days), std::nullopt);

    SplitMix rng{seed ^ (static_cast<std::uint64_t>(period) * 0xD1B54A32D192ED03ULL)};
    rng.next(); // decorrelate neighbouring seeds
    const double u_wet = rng.uniform();
    const double u_kind = rng.uniform();
    const double u_type = rng.uniform();
    // Irwin-Hall sum of four uniforms, scaled to unit variance: a smooth,
    // bounded stand-in for a normal anomaly built from exact arithmetic only.
    double sum = 0.0;
    for (int i = 0; i < 4; ++i) sum += rng.uniform();
    const double z = (sum - 2.0) * std::sqrt(3.0);

    WeatherDay day;
    day.temperature_anomaly_c = static_cast<float>(z * climate.temperature_variability_c);
    const float mean_t = sample.mean_temperature_c + day.temperature_anomaly_c;
    if (u_wet < sample.precipitation_chance) {
        day.type = u_type < snow_fraction(mean_t) ? PrecipitationType::snow : PrecipitationType::rain;
        if (u_kind < k_showers_share) {
            day.kind = WeatherKind::showers;
        } else if (u_kind < k_showers_share + k_steady_share) {
            day.kind = day.type == PrecipitationType::snow ? WeatherKind::snow : WeatherKind::rain;
        } else {
            day.kind = WeatherKind::storm;
        }
    } else {
        const double cover = sample.cloud_cover + (u_kind - 0.5) * k_dry_cloud_spread;
        day.kind = cover < 0.3 ? WeatherKind::clear : (cover < 0.7 ? WeatherKind::fair : WeatherKind::overcast);
    }
    return day;
}

WeatherState weather_for_day(const WeatherDay& day, const ClimateSample& sample, const WeatherState& base) {
    const KindLook& look = look_of(day.kind);
    WeatherState    s = base;
    s.cloud_cover = look.cloud;
    s.precipitation = look.precipitation;
    s.precipitation_type = day.type;
    s.wind_speed = std::clamp(sample.wind_speed * look.wind_factor, 0.0F, 60.0F);
    s.gustiness = look.gustiness;
    s.turbulence = look.turbulence;
    s.temperature_c =
        std::clamp(air_temperature(sample, day.temperature_anomaly_c, look.cloud), -40.0F, 45.0F);
    return s;
}

WeatherState weather_preset(WeatherKind kind, const ClimateSample& sample, const WeatherState& base) {
    WeatherDay day{.kind = kind, .type = PrecipitationType::rain, .temperature_anomaly_c = 0.0F};
    const float t = air_temperature(sample, 0.0F, look_of(kind).cloud);
    if (kind == WeatherKind::snow) {
        day.type = PrecipitationType::snow;
    } else if (kind == WeatherKind::showers || kind == WeatherKind::storm) {
        day.type = snow_fraction(t) >= 0.5F ? PrecipitationType::snow : PrecipitationType::rain;
    }
    WeatherState s = weather_for_day(day, sample, base);
    if (kind == WeatherKind::rain) s.temperature_c = std::max(s.temperature_c, k_rain_min_temperature_c);
    if (kind == WeatherKind::snow) s.temperature_c = std::min(s.temperature_c, k_snow_preset_max_temperature_c);
    return s;
}

AutoWeather auto_weather(const Climate& climate, std::uint64_t seed, double season_days,
                         const ClimateSample& sample, const WeatherState& base) {
    AutoWeather out;
    const double x = (std::isfinite(season_days) ? season_days : 0.0) / climate.weather_period_days;
    const double fx = std::floor(x);
    out.period = static_cast<std::int64_t>(fx);
    out.day = weather_day(climate, seed, out.period);
    out.state = weather_for_day(out.day, sample, base);

    const double f = x - fx;
    const double tr = climate.transition;
    if (tr <= 0.0 || f <= 1.0 - tr) return out;
    // Ease into the next period (smoothstep over the transition window).
    const auto   u = static_cast<float>(std::clamp((f - (1.0 - tr)) / tr, 0.0, 1.0));
    const float  b = u * u * (3.0F - 2.0F * u);
    const WeatherDay   next_day = weather_day(climate, seed, out.period + 1);
    const WeatherState next = weather_for_day(next_day, sample, base);
    out.blend = b;
    WeatherState& s = out.state;
    s.cloud_cover = lerp(s.cloud_cover, next.cloud_cover, b);
    s.precipitation = lerp(s.precipitation, next.precipitation, b);
    s.wind_speed = lerp(s.wind_speed, next.wind_speed, b);
    s.gustiness = lerp(s.gustiness, next.gustiness, b);
    s.turbulence = lerp(s.turbulence, next.turbulence, b);
    s.temperature_c = lerp(s.temperature_c, next.temperature_c, b);
    // What falls: whichever period contributes precipitation, the nearer one
    // when both do.
    const bool wet_now = precipitating(out.day.kind);
    const bool wet_next = precipitating(next_day.kind);
    if (wet_next && (!wet_now || b >= 0.5F)) s.precipitation_type = next.precipitation_type;
    return out;
}

} // namespace engine::weather
