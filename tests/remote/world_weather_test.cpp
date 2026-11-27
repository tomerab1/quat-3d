// weather.get / weather.set (R13.1) against a real Scene.

#include <cmath>
#include <string>

#include <gtest/gtest.h>

#include "engine/weather/weather_state.hpp"
#include "engine/weather/wind_field.hpp"
#include "remote/world_fixture.hpp"

namespace {

using quat::test::Json;
namespace code = engine::remote::rpc_code;
namespace w = engine::weather;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

class WorldWeather : public quat::test::WorldCommands {
protected:
    w::WindAdvection clock{.distance_m = 250.0, .time_s = 50.0};
    void SetUp() override {
        services.wind_clock = [this] { return clock; };
        register_commands();
    }
};

TEST_F(WorldWeather, GetWithoutWeatherReportsTheDefaults) {
    const Json r = ok("weather.get");
    EXPECT_FALSE(r["present"].get<bool>());
    EXPECT_TRUE(r["entity"].is_null());
    EXPECT_FLOAT_EQ(r["weather"]["wind_speed"].get<float>(), w::WeatherState{}.wind_speed);
    EXPECT_EQ(r["weather"]["precipitation_type"], "rain");
    EXPECT_FALSE(r.contains("schema"));
}

TEST_F(WorldWeather, SetCreatesTheSingletonThenMerges) {
    Json r = ok("weather.set", {{"precipitation", 0.8}, {"wind_speed", 14}});
    EXPECT_TRUE(r["present"].get<bool>());
    const entt::entity e = entity(r["entity"]);
    ASSERT_TRUE(reg().all_of<w::WeatherState>(e));
    EXPECT_FLOAT_EQ(reg().get<w::WeatherState>(e).precipitation, 0.8F);

    r = ok("weather.set", {{"precipitation_type", "snow"}, {"temperature_c", -4}});
    EXPECT_EQ(entity(r["entity"]), e); // same entity, merged
    EXPECT_FLOAT_EQ(r["weather"]["wind_speed"].get<float>(), 14.0F);
    EXPECT_EQ(r["weather"]["precipitation_type"], "snow");
    EXPECT_EQ(reg().view<w::WeatherState>().size(), 1U);

    r = ok("weather.set", {{"reset", true}, {"cloud_cover", 0.1}});
    EXPECT_FLOAT_EQ(r["weather"]["wind_speed"].get<float>(), w::WeatherState{}.wind_speed);
    EXPECT_FLOAT_EQ(r["weather"]["cloud_cover"].get<float>(), 0.1F);
}

TEST_F(WorldWeather, SetRejectsBadFieldsWithoutChangingAnything) {
    ok("weather.set", {{"wetness", 0.3}});
    std::string m = err("weather.set", {{"wetness", 0.9}, {"wind_speed", -1}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "weather.wind_speed: expected a number in [0, 60] (m/s)")) << m;
    m = err("weather.set", {{"fog", 1}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "weather.fog: unknown field (known: ")) << m;
    m = err("weather.set", {{"precipitation_type", "sleet"}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "\"rain\", \"snow\"")) << m;
    m = err("weather.set", {{"reset", "yes"}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "reset: expected true or false")) << m;
    EXPECT_FLOAT_EQ(ok("weather.get")["weather"]["wetness"].get<float>(), 0.3F);
}

TEST_F(WorldWeather, SchemaAndWindQueries) {
    ok("weather.set", {{"wind_speed", 10}, {"wind_dir_deg", 0}, {"gustiness", 0.5}});
    const Json r = ok("weather.get", {{"schema", true}, {"wind_at", {{5, 10, 7}, {5, 0.5, 7}}}});
    ASSERT_TRUE(r["schema"].is_array());
    EXPECT_EQ(r["schema"].size(), w::weather_fields().size());
    ASSERT_EQ(r["wind_at"].size(), 2U);

    // The answer is the CPU reference at the application's clock.
    const w::WeatherState& state = reg().get<w::WeatherState>(entity(r["entity"]));
    const w::WindSample ref = w::sample_wind(w::wind_field_params(state, clock), {5.0F, 7.0F});
    EXPECT_NEAR(r["wind_at"][0][0].get<float>(), ref.velocity.x, 1e-5F);
    EXPECT_NEAR(r["wind_at"][0][2].get<float>(), ref.velocity.z, 1e-5F);
    // Lower down the log profile slows it.
    EXPECT_LT(std::abs(r["wind_at"][1][0].get<float>()), std::abs(r["wind_at"][0][0].get<float>()));

    std::string m = err("weather.get", {{"wind_at", {{1, 2}}}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "wind_at[0]: expected [x, y, z]")) << m;
    m = err("weather.get", {{"verbose", true}}, code::invalid_params);
    EXPECT_TRUE(contains(m, "unknown field")) << m;
}

TEST_F(WorldWeather, SceneCodecSharesTheSingletonRule) {
    ok("weather.set", {{"wind_speed", 2}});
    const std::string m = err("scene.create",
                              {{"name", "second"}, {"components", {{"weather", Json::object()}}}},
                              code::invalid_params);
    EXPECT_TRUE(contains(m, "already has its weather on entity")) << m;
    EXPECT_EQ(reg().view<w::WeatherState>().size(), 1U);
}

} // namespace
