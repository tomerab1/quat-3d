// Climate curves and sampling — see include/engine/weather/climate.hpp.

#include "engine/weather/climate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace engine::weather {

namespace {

constexpr std::array k_curves = {
    ClimateCurveDesc{ClimateCurve::temperature_c, "temperature_c", -60.0F, 50.0F, "C",
                     "Daily mean air temperature"},
    ClimateCurveDesc{ClimateCurve::diurnal_range_c, "diurnal_range_c", 0.0F, 30.0F, "C",
                     "Daily maximum minus minimum on a clear day"},
    ClimateCurveDesc{ClimateCurve::precipitation_chance, "precipitation_chance", 0.0F, 1.0F, "",
                     "Chance that a weather period brings rain or snow"},
    ClimateCurveDesc{ClimateCurve::cloud_cover, "cloud_cover", 0.0F, 1.0F, "",
                     "Mean cloud cover of the dry periods"},
    ClimateCurveDesc{ClimateCurve::wind_speed, "wind_speed", 0.0F, 40.0F, "m/s",
                     "Mean wind speed 10 m above the ground"},
};
static_assert(k_curves.size() == k_climate_curve_count);

[[nodiscard]] double wrap01(double x) {
    if (!std::isfinite(x)) return 0.0;
    const double f = x - std::floor(x);
    return f >= 1.0 ? 0.0 : f;
}

[[nodiscard]] float clamp_or(float v, float lo, float hi, float fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

} // namespace

std::span<const ClimateCurveDesc> climate_curves() { return k_curves; }

namespace {

// Both constnesses of curve_keys().
template <typename C>
auto& keys_of(C& c, ClimateCurve curve) {
    switch (curve) {
    case ClimateCurve::temperature_c: return c.temperature_c;
    case ClimateCurve::diurnal_range_c: return c.diurnal_range_c;
    case ClimateCurve::precipitation_chance: return c.precipitation_chance;
    case ClimateCurve::cloud_cover: return c.cloud_cover;
    case ClimateCurve::wind_speed: break;
    }
    return c.wind_speed;
}

} // namespace

std::vector<float>& curve_keys(Climate& c, ClimateCurve curve) { return keys_of(c, curve); }

const std::vector<float>& curve_keys(const Climate& c, ClimateCurve curve) { return keys_of(c, curve); }

float sample_curve(std::span<const float> keys, double phase) {
    const std::size_t n = keys.size();
    if (n == 0) return 0.0F;
    if (n == 1) return keys[0];
    // Periodic uniform Catmull-Rom (Catmull & Rom 1974): through every key,
    // C1, and exact for a constant curve.
    const double      x = wrap01(phase) * static_cast<double>(n);
    const auto        i1 = static_cast<std::size_t>(std::floor(x)) % n;
    const double      t = x - std::floor(x);
    const std::size_t i0 = (i1 + n - 1) % n;
    const std::size_t i2 = (i1 + 1) % n;
    const std::size_t i3 = (i1 + 2) % n;
    const double      p0 = keys[i0];
    const double      p1 = keys[i1];
    const double      p2 = keys[i2];
    const double      p3 = keys[i3];
    const double      t2 = t * t;
    const double      t3 = t2 * t;
    const double v = 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                            (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
    return static_cast<float>(v);
}

Climate sanitized(const Climate& climate) {
    const Climate defaults;
    Climate       c = climate;
    for (const ClimateCurveDesc& d : k_curves) {
        std::vector<float>& keys = curve_keys(c, d.curve);
        if (keys.empty() || keys.size() > k_climate_max_keys) keys = curve_keys(defaults, d.curve);
        for (float& k : keys) k = clamp_or(k, d.min, d.max, 0.5F * (d.min + d.max));
    }
    c.temperature_variability_c =
        clamp_or(c.temperature_variability_c, 0.0F, 15.0F, defaults.temperature_variability_c);
    c.weather_period_days = clamp_or(c.weather_period_days, 0.25F, 60.0F, defaults.weather_period_days);
    c.transition = clamp_or(c.transition, 0.0F, 1.0F, defaults.transition);
    return c;
}

ClimateSample sample_climate(const Climate& climate, double phase, std::optional<double> solar_hour) {
    ClimateSample s;
    // Interpolated values may overshoot between keys: clamp to each range.
    const auto at = [&](ClimateCurve curve) {
        const ClimateCurveDesc& d = k_curves[static_cast<std::size_t>(curve)];
        return std::clamp(sample_curve(curve_keys(climate, curve), phase), d.min, d.max);
    };
    s.mean_temperature_c = at(ClimateCurve::temperature_c);
    s.diurnal_range_c = at(ClimateCurve::diurnal_range_c);
    s.precipitation_chance = at(ClimateCurve::precipitation_chance);
    s.cloud_cover = at(ClimateCurve::cloud_cover);
    s.wind_speed = at(ClimateCurve::wind_speed);
    if (solar_hour && std::isfinite(*solar_hour)) {
        // One harmonic of Parton & Logan's (1981) diurnal course: the maximum
        // a few hours after solar noon, the minimum near sunrise.
        s.diurnal = static_cast<float>(
            std::cos(2.0 * std::numbers::pi * (*solar_hour - k_diurnal_max_hour) / 24.0));
    }
    return s;
}

float air_temperature(const ClimateSample& s, float anomaly_c, float cloud_cover) {
    const float damping = 1.0F - 0.5F * std::clamp(cloud_cover, 0.0F, 1.0F);
    return s.mean_temperature_c + anomaly_c + 0.5F * s.diurnal_range_c * s.diurnal * damping;
}

float snow_fraction(float t_c) {
    // Dai (2008), GRL 35 L12802, Table 1, land, annual: snow frequency (%) =
    // a (tanh(b (T - c)) - d) with a = -48.2292, b = 0.7205, c = 1.1662,
    // d = 1.0223, T the 2 m air temperature in C.
    constexpr double a = -48.2292;
    constexpr double b = 0.7205;
    constexpr double c = 1.1662;
    constexpr double d = 1.0223;
    if (!std::isfinite(t_c)) return 0.0F;
    const double percent = a * (std::tanh(b * (static_cast<double>(t_c) - c)) - d);
    return static_cast<float>(std::clamp(percent / 100.0, 0.0, 1.0));
}

} // namespace engine::weather
