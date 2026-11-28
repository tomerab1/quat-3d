// Climate (R25.1): curves, sampling, the rain / snow split, presets and the
// seeded weather sequence.

#include <cmath>
#include <cstdint>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include "engine/weather/climate.hpp"

namespace {

namespace w = engine::weather;

[[nodiscard]] w::Climate constant_climate(float t, float precip, float cloud) {
    w::Climate c;
    c.temperature_c = {t};
    c.diurnal_range_c = {8.0F};
    c.precipitation_chance = {precip};
    c.cloud_cover = {cloud};
    c.wind_speed = {4.0F};
    return c;
}

TEST(ClimateCurve, PassesThroughEveryKeyAndWrapsSmoothly) {
    const std::vector<float> keys = {1.0F, 4.0F, -2.0F, 7.0F, 3.0F};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        EXPECT_NEAR(w::sample_curve(keys, static_cast<double>(i) / 5.0), keys[i], 1e-5F) << i;
    }
    // Periodic: phase 1 is phase 0, and the curve is continuous across it.
    EXPECT_NEAR(w::sample_curve(keys, 1.0), keys[0], 1e-5F);
    EXPECT_NEAR(w::sample_curve(keys, -0.2), w::sample_curve(keys, 0.8), 1e-5F);
    EXPECT_NEAR(w::sample_curve(keys, 0.999999), w::sample_curve(keys, 0.0), 1e-3F);
    // A constant curve stays constant (no overshoot).
    const std::vector<float> flat(12, 5.5F);
    for (double p = 0.0; p < 1.0; p += 0.013) EXPECT_FLOAT_EQ(w::sample_curve(flat, p), 5.5F);
    EXPECT_EQ(w::sample_curve({}, 0.3), 0.0F);
}

TEST(ClimateCurve, TableCoversEveryCurveOnceAndDefaultsAreInRange) {
    std::set<std::string> names;
    const w::Climate      c;
    for (const w::ClimateCurveDesc& d : w::climate_curves()) {
        EXPECT_TRUE(names.insert(std::string(d.name)).second) << d.name;
        const std::vector<float>& keys = w::curve_keys(c, d.curve);
        EXPECT_FALSE(keys.empty()) << d.name;
        for (const float k : keys) {
            EXPECT_GE(k, d.min) << d.name;
            EXPECT_LE(k, d.max) << d.name;
        }
    }
    EXPECT_EQ(names.size(), w::k_climate_curve_count);
}

TEST(ClimateCurve, SanitizedClampsAndRestoresBrokenCurves) {
    w::Climate c;
    c.temperature_c = {};                       // empty: the default curve
    c.cloud_cover = {2.0F, -1.0F, NAN};         // clamped, NaN -> mid-range
    c.wind_speed = std::vector<float>(100, 3.0F); // too long: the default
    c.weather_period_days = -4.0F;
    c.transition = NAN;
    const w::Climate s = w::sanitized(c);
    EXPECT_EQ(s.temperature_c, w::Climate{}.temperature_c);
    ASSERT_EQ(s.cloud_cover.size(), 3U);
    EXPECT_EQ(s.cloud_cover[0], 1.0F);
    EXPECT_EQ(s.cloud_cover[1], 0.0F);
    EXPECT_EQ(s.cloud_cover[2], 0.5F);
    EXPECT_EQ(s.wind_speed, w::Climate{}.wind_speed);
    EXPECT_EQ(s.weather_period_days, 0.25F);
    EXPECT_EQ(s.transition, w::Climate{}.transition);
}

TEST(ClimateDefault, ColdestInMidWinterWarmestAfterTheSummerSolstice) {
    const w::Climate c;
    double coldest = 0.0;
    double warmest = 0.0;
    float  t_min = 1e9F;
    float  t_max = -1e9F;
    for (int i = 0; i < 1000; ++i) {
        const double p = i / 1000.0;
        const float  t = w::sample_climate(c, p, std::nullopt).mean_temperature_c;
        if (t < t_min) {
            t_min = t;
            coldest = p;
        }
        if (t > t_max) {
            t_max = t;
            warmest = p;
        }
    }
    // Thermal lag: about a month after the solstices (0.25 / 0.75).
    EXPECT_GT(warmest, 0.27);
    EXPECT_LT(warmest, 0.42);
    EXPECT_GT(coldest, 0.77);
    EXPECT_LT(coldest, 0.92);
    // Snow is likely from mid-winter (the mean dips below freezing) and the
    // summer is mild.
    EXPECT_LT(t_min, 0.0F);
    EXPECT_GT(t_max, 15.0F);
    EXPECT_LT(t_max, 22.0F);
    // A cloudier, windier winter half.
    EXPECT_GT(w::sample_climate(c, 0.85, std::nullopt).cloud_cover,
              w::sample_climate(c, 0.35, std::nullopt).cloud_cover);
    EXPECT_GT(w::sample_climate(c, 0.85, std::nullopt).wind_speed,
              w::sample_climate(c, 0.35, std::nullopt).wind_speed);
}

TEST(ClimateDiurnal, PeaksMidAfternoonAndClearNightsAreColder) {
    const w::Climate c = constant_climate(10.0F, 0.3F, 0.5F);
    const auto at = [&](double h, float cloud) {
        return w::air_temperature(w::sample_climate(c, 0.1, h), 0.0F, cloud);
    };
    EXPECT_NEAR(at(w::k_diurnal_max_hour, 0.0F), 14.0F, 1e-4F); // mean + range / 2
    EXPECT_NEAR(at(w::k_diurnal_max_hour - 12.0, 0.0F), 6.0F, 1e-4F);
    EXPECT_GT(at(14.0, 0.0F), at(9.0, 0.0F));
    // Overcast halves the swing; without a time of day, the mean.
    EXPECT_NEAR(at(w::k_diurnal_max_hour - 12.0, 1.0F), 8.0F, 1e-4F);
    EXPECT_FLOAT_EQ(w::air_temperature(w::sample_climate(c, 0.1, std::nullopt), 1.5F, 0.0F), 11.5F);
}

TEST(ClimateSnow, DaiRainSnowTransition) {
    // Dai (2008), land: 50 % snow at +1.17 C, almost all below -2 C, almost
    // none above +6 C, monotone in between; certain at the extremes.
    EXPECT_NEAR(w::snow_fraction(1.1662F), 0.5F, 1e-4F);
    EXPECT_GT(w::snow_fraction(-2.0F), 0.95F);
    EXPECT_GT(w::snow_fraction(-30.0F), 0.9999F);
    EXPECT_LT(w::snow_fraction(30.0F), 1e-6F);
    EXPECT_LT(w::snow_fraction(6.0F), 0.03F);
    float prev = 2.0F;
    for (float t = -20.0F; t <= 20.0F; t += 0.25F) {
        const float f = w::snow_fraction(t);
        EXPECT_GE(f, 0.0F);
        EXPECT_LE(f, 1.0F);
        EXPECT_LE(f, prev) << t;
        prev = f;
    }
    EXPECT_EQ(w::snow_fraction(NAN), 0.0F);
}

TEST(WeatherKinds, NamesRoundTrip) {
    for (const std::string_view n : w::weather_kind_names()) {
        w::WeatherKind k{};
        ASSERT_TRUE(w::parse_weather_kind(n, k)) << n;
        EXPECT_EQ(w::weather_kind_name(k), n);
    }
    w::WeatherKind k{};
    EXPECT_FALSE(w::parse_weather_kind("hail", k));
    EXPECT_EQ(w::weather_kind_names().size(), w::k_weather_kind_count);
}

TEST(WeatherPreset, PrecipitationFollowsTheSeasonsTemperature) {
    w::WeatherState base;
    base.wetness = 0.4F;
    base.snow = 0.2F;
    base.wind_dir_deg = 100.0F;
    base.accumulate = false;
    const auto sample = [](float t) {
        return w::sample_climate(constant_climate(t, 0.4F, 0.6F), 0.5, std::nullopt);
    };
    // Showers and storms: snow in the cold, rain in the warm.
    EXPECT_EQ(w::weather_preset(w::WeatherKind::showers, sample(-5.0F), base).precipitation_type,
              w::PrecipitationType::snow);
    EXPECT_EQ(w::weather_preset(w::WeatherKind::showers, sample(12.0F), base).precipitation_type,
              w::PrecipitationType::rain);
    EXPECT_EQ(w::weather_preset(w::WeatherKind::storm, sample(-8.0F), base).precipitation_type,
              w::PrecipitationType::snow);
    // Forced kinds move the temperature to where they can fall.
    const w::WeatherState rain = w::weather_preset(w::WeatherKind::rain, sample(-5.0F), base);
    EXPECT_EQ(rain.precipitation_type, w::PrecipitationType::rain);
    EXPECT_GE(rain.temperature_c, 2.0F);
    const w::WeatherState snow = w::weather_preset(w::WeatherKind::snow, sample(15.0F), base);
    EXPECT_EQ(snow.precipitation_type, w::PrecipitationType::snow);
    EXPECT_LE(snow.temperature_c, -1.0F);
    EXPECT_GT(snow.precipitation, 0.0F);
    // A clear day: no precipitation, few clouds, the climate's temperature.
    const w::WeatherState clear = w::weather_preset(w::WeatherKind::clear, sample(20.0F), base);
    EXPECT_EQ(clear.precipitation, 0.0F);
    EXPECT_LT(clear.cloud_cover, 0.2F);
    EXPECT_FLOAT_EQ(clear.temperature_c, 20.0F);
    // Storms blow harder than calm days.
    EXPECT_GT(w::weather_preset(w::WeatherKind::storm, sample(10.0F), base).wind_speed,
              2.0F * clear.wind_speed);
    // Surfaces and the wind direction are the scene's, not the preset's.
    for (const w::WeatherState& s : {rain, snow, clear}) {
        EXPECT_EQ(s.wetness, base.wetness);
        EXPECT_EQ(s.snow, base.snow);
        EXPECT_EQ(s.wind_dir_deg, base.wind_dir_deg);
        EXPECT_EQ(s.accumulate, base.accumulate);
    }
}

TEST(WeatherSequence, IsAPureFunctionOfSeedAndPeriod) {
    const w::Climate c;
    for (std::int64_t p = -50; p < 200; ++p) {
        EXPECT_EQ(w::weather_day(c, 7, p), w::weather_day(c, 7, p)) << p;
    }
    int differ = 0;
    for (std::int64_t p = 0; p < 200; ++p) differ += w::weather_day(c, 7, p) != w::weather_day(c, 8, p);
    EXPECT_GT(differ, 150) << "another seed gives another sequence";
}

TEST(WeatherSequence, FrequenciesFollowTheClimate) {
    // Constant curves: the shares do not depend on where the period falls.
    const w::Climate wet = constant_climate(12.0F, 0.7F, 0.5F);
    const w::Climate dry = constant_climate(12.0F, 0.1F, 0.5F);
    const auto wet_share = [](const w::Climate& c) {
        int n = 0;
        for (std::int64_t p = 0; p < 4000; ++p) {
            const w::WeatherKind k = w::weather_day(c, 3, p).kind;
            n += k == w::WeatherKind::showers || k == w::WeatherKind::rain ||
                 k == w::WeatherKind::storm || k == w::WeatherKind::snow;
        }
        return n / 4000.0;
    };
    EXPECT_NEAR(wet_share(wet), 0.7, 0.03);
    EXPECT_NEAR(wet_share(dry), 0.1, 0.03);

    // Snow only where it is cold enough.
    // (No anomalies: a 15 C day can otherwise dip to a few degrees, where
    // Dai's curve still gives a few percent of snow.)
    w::Climate warm = constant_climate(15.0F, 0.6F, 0.5F);
    w::Climate cold = constant_climate(-8.0F, 0.6F, 0.5F);
    warm.temperature_variability_c = cold.temperature_variability_c = 0.0F;
    int warm_snow = 0;
    int cold_snow = 0;
    int cold_wet = 0;
    for (std::int64_t p = 0; p < 2000; ++p) {
        warm_snow += w::weather_day(warm, 5, p).kind == w::WeatherKind::snow;
        const w::WeatherDay d = w::weather_day(cold, 5, p);
        if (d.kind == w::WeatherKind::showers || d.kind == w::WeatherKind::rain ||
            d.kind == w::WeatherKind::storm || d.kind == w::WeatherKind::snow) {
            ++cold_wet;
            cold_snow += d.type == w::PrecipitationType::snow;
        }
    }
    EXPECT_EQ(warm_snow, 0);
    EXPECT_GT(cold_snow, cold_wet * 9 / 10);

    // Anomalies spread with temperature_variability_c (unit-variance draws).
    w::Climate c = warm;
    c.temperature_variability_c = 4.0F;
    double sum2 = 0.0;
    for (std::int64_t p = 0; p < 4000; ++p) {
        const double a = w::weather_day(c, 9, p).temperature_anomaly_c;
        sum2 += a * a;
    }
    EXPECT_NEAR(std::sqrt(sum2 / 4000.0), 4.0, 0.25);
}

TEST(WeatherSequence, EasesContinuouslyIntoTheNextPeriod) {
    const w::Climate      c;
    const w::ClimateSample sample = w::sample_climate(c, 0.8, 14.0);
    const w::WeatherState  base;
    const double           period = c.weather_period_days;
    for (std::int64_t k = 10; k < 60; ++k) {
        const double         edge = static_cast<double>(k) * period;
        const w::AutoWeather before = w::auto_weather(c, 11, edge - 1e-7, sample, base);
        const w::AutoWeather after = w::auto_weather(c, 11, edge, sample, base);
        EXPECT_EQ(after.period, k);
        EXPECT_NEAR(before.state.cloud_cover, after.state.cloud_cover, 1e-4F) << k;
        EXPECT_NEAR(before.state.precipitation, after.state.precipitation, 1e-4F) << k;
        EXPECT_NEAR(before.state.wind_speed, after.state.wind_speed, 1e-3F) << k;
        EXPECT_NEAR(before.state.temperature_c, after.state.temperature_c, 1e-3F) << k;
        // Before the transition window: the period's own weather.
        const w::AutoWeather mid = w::auto_weather(c, 11, edge + 0.25 * period, sample, base);
        EXPECT_EQ(mid.blend, 0.0F);
        const w::WeatherState own = w::weather_for_day(mid.day, sample, base);
        EXPECT_EQ(mid.state.cloud_cover, own.cloud_cover);
        EXPECT_EQ(mid.state.precipitation, own.precipitation);
        EXPECT_EQ(mid.state.temperature_c, own.temperature_c);
    }
}

} // namespace
