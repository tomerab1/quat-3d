// WeatherState (R13.1): field table, sanitizing and accumulation.

#include <cmath>
#include <limits>
#include <set>
#include <string>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "engine/weather/weather_state.hpp"

namespace {

namespace w = engine::weather;

TEST(WeatherTable, EveryFieldIsListedOnceWithItsStructDefault) {
    std::set<std::string> names;
    for (const w::WeatherFieldDesc& d : w::weather_fields()) {
        EXPECT_TRUE(names.insert(std::string(d.name)).second) << d.name;
        EXPECT_LE(d.min, d.max) << d.name;
        w::WeatherState s;
        const w::WeatherFieldRef r = d.ref(s);
        const int set = (r.f != nullptr) + (r.b != nullptr) + (r.p != nullptr);
        EXPECT_EQ(set, 1) << d.name;
        if (r.f != nullptr) {
            EXPECT_EQ(*r.f, d.default_value) << d.name;
            EXPECT_EQ(d.type, w::WeatherFieldType::number) << d.name;
            EXPECT_GE(d.default_value, d.min) << d.name;
            EXPECT_LE(d.default_value, d.max) << d.name;
        }
        if (r.b != nullptr) EXPECT_EQ(*r.b ? 1.0F : 0.0F, d.default_value) << d.name;
        EXPECT_FALSE(d.hint.empty()) << d.name;
    }
    // Every WeatherState member: 11 fields.
    EXPECT_EQ(names.size(), 11U);
    EXPECT_NE(w::find_weather_field("wind_speed"), nullptr);
    EXPECT_EQ(w::find_weather_field("windspeed"), nullptr);
}

TEST(WeatherTable, PrecipitationTypeNamesRoundTrip) {
    for (const w::PrecipitationType t : {w::PrecipitationType::rain, w::PrecipitationType::snow}) {
        w::PrecipitationType back = w::PrecipitationType::rain;
        ASSERT_TRUE(w::parse_precipitation_type(w::precipitation_type_name(t), back));
        EXPECT_EQ(back, t);
    }
    w::PrecipitationType out = w::PrecipitationType::snow;
    EXPECT_FALSE(w::parse_precipitation_type("hail", out));
    EXPECT_EQ(out, w::PrecipitationType::snow);
}

TEST(WeatherTable, SanitizedClampsAndReplacesNonFinite) {
    w::WeatherState s;
    s.wind_speed = 500.0F;
    s.wetness = -1.0F;
    s.cloud_cover = std::numeric_limits<float>::quiet_NaN();
    s.precipitation_type = static_cast<w::PrecipitationType>(7);
    const w::WeatherState c = w::sanitized(s);
    EXPECT_EQ(c.wind_speed, 60.0F);
    EXPECT_EQ(c.wetness, 0.0F);
    EXPECT_EQ(c.cloud_cover, 0.42F);
    EXPECT_EQ(c.precipitation_type, w::PrecipitationType::rain);
}

TEST(WeatherAccumulation, RainSoaksAndDryingIsSlow) {
    w::WeatherState s;
    s.precipitation = 1.0F;
    w::step_weather(s, w::k_wetting_time_s);
    EXPECT_NEAR(s.wetness, 1.0F - std::exp(-1.0F), 1e-4F);
    // Frame-rate independent: many small steps reach the same state.
    w::WeatherState t;
    t.precipitation = 1.0F;
    for (int i = 0; i < 900; ++i) w::step_weather(t, w::k_wetting_time_s / 900.0F);
    EXPECT_NEAR(t.wetness, s.wetness, 1e-4F);

    s.precipitation = 0.0F;
    const float wet = s.wetness;
    w::step_weather(s, 60.0F);
    EXPECT_LT(s.wetness, wet);
    EXPECT_GT(s.wetness, wet * 0.9F); // a minute of sun does not dry a soaked street
}

TEST(WeatherAccumulation, SnowSettlesWhenColdAndMeltsIntoWetnessWhenWarm) {
    w::WeatherState s;
    s.precipitation = 1.0F;
    s.precipitation_type = w::PrecipitationType::snow;
    s.temperature_c = -5.0F;
    w::step_weather(s, 600.0F);
    EXPECT_GT(s.snow, 0.8F);
    EXPECT_EQ(s.wetness, 0.0F); // dry snow does not wet anything

    // Too warm to settle.
    w::WeatherState warm = s;
    warm.snow = 0.0F;
    warm.temperature_c = 6.0F;
    w::step_weather(warm, 600.0F);
    EXPECT_EQ(warm.snow, 0.0F);

    // Thaw: the cover shrinks and its meltwater wets the ground.
    s.precipitation = 0.0F;
    s.temperature_c = 10.0F;
    const float snow = s.snow;
    w::step_weather(s, 300.0F);
    EXPECT_LT(s.snow, snow);
    EXPECT_GT(s.wetness, 0.1F);
}

TEST(WeatherAccumulation, OffOrZeroDtLeavesTheStateAlone) {
    w::WeatherState s;
    s.precipitation = 1.0F;
    s.accumulate = false;
    w::step_weather(s, 100.0F);
    EXPECT_EQ(s.wetness, 0.0F);
    s.accumulate = true;
    w::step_weather(s, 0.0F);
    w::step_weather(s, -1.0F);
    EXPECT_EQ(s.wetness, 0.0F);
}

TEST(WeatherSystem, StepsTheSceneSingleton) {
    entt::registry r;
    EXPECT_EQ(w::find_weather(r), entt::entity{entt::null});
    w::weather_system(r, 1.0F); // no weather: no-op
    const entt::entity e = r.create();
    r.emplace<w::WeatherState>(e).precipitation = 1.0F;
    EXPECT_EQ(w::find_weather(r), e);
    w::weather_system(r, 10.0F);
    EXPECT_GT(r.get<w::WeatherState>(e).wetness, 0.0F);
}

} // namespace
