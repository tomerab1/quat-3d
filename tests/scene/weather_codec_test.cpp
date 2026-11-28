// Weather component codec (R13.1): scene files, scene.* and weather.*.

#include <filesystem>
#include <string>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_io.hpp"
#include "engine/scene/weather_json.hpp"
#include "engine/weather/weather_state.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

using json = nlohmann::json;
namespace s = engine::scene;
namespace w = engine::weather;

class WeatherCodec : public ::testing::Test {
protected:
    entt::registry r;
    entt::entity   e = r.create();

    [[nodiscard]] auto apply(const json& v, bool strict = true) {
        return s::apply_component(r, e, "weather", v, strict);
    }
    [[nodiscard]] const w::WeatherState& weather() const { return r.get<w::WeatherState>(e); }
};

TEST_F(WeatherCodec, RoundTripsEveryField) {
    const json authored = {{"wind_dir_deg", -120.0},  {"wind_speed", 18.5},
                           {"gustiness", 0.9},        {"turbulence", 0.6},
                           {"cloud_cover", 0.95},     {"precipitation", 0.7},
                           {"precipitation_type", "snow"}, {"wetness", 0.25},
                           {"snow", 0.8},             {"temperature_c", -6.0},
                           {"accumulate", false}};
    ASSERT_OK(apply(authored));
    EXPECT_EQ(weather().precipitation_type, w::PrecipitationType::snow);
    EXPECT_FLOAT_EQ(weather().wind_speed, 18.5F);
    EXPECT_FALSE(weather().accumulate);

    const json encoded = s::encode_components(r, e)["weather"];
    EXPECT_EQ(encoded.size(), w::weather_fields().size());
    for (const auto& [key, value] : authored.items()) {
        if (value.is_number()) {
            EXPECT_FLOAT_EQ(encoded[key].get<float>(), value.get<float>()) << key;
        } else {
            EXPECT_EQ(encoded[key], value) << key;
        }
    }
    // Decoding what was encoded changes nothing.
    const w::WeatherState before = weather();
    ASSERT_OK(apply(encoded));
    EXPECT_EQ(s::encode_weather(weather()), s::encode_weather(before));
    EXPECT_EQ(s::component_names(r, e), std::vector<std::string_view>{"weather"});
}

TEST_F(WeatherCodec, MergesPartialUpdates) {
    ASSERT_OK(apply({{"wind_speed", 9.0}}));
    ASSERT_OK(apply({{"precipitation", 0.4}, {"wind_dir_deg", nullptr}}));
    EXPECT_FLOAT_EQ(weather().wind_speed, 9.0F);
    EXPECT_FLOAT_EQ(weather().precipitation, 0.4F);
    EXPECT_FLOAT_EQ(weather().wind_dir_deg, w::WeatherState{}.wind_dir_deg);
}

TEST_F(WeatherCodec, StrictErrorsNameTheFieldAndWhatItTakes) {
    ASSERT_OK(apply({{"wind_speed", 5.0}}));
    const auto expect_error = [&](const json& v, const std::string& needle) {
        const auto r2 = apply(v);
        ASSERT_FALSE(r2.has_value()) << v.dump();
        EXPECT_NE(r2.error().message.find(needle), std::string::npos) << r2.error().message;
        EXPECT_FLOAT_EQ(weather().wind_speed, 5.0F) << "nothing changes on error";
    };
    expect_error({{"wind_speed", 1000.0}, {"wetness", 0.5}}, "weather.wind_speed: expected a number in [0, 60] (m/s)");
    expect_error({{"wind_speed", 7.0}, {"rain", 1.0}}, "weather.rain: unknown field (known: wind_dir_deg");
    expect_error({{"precipitation_type", "hail"}}, "expected one of \"rain\", \"snow\"");
    expect_error({{"accumulate", 1}}, "weather.accumulate: expected true or false");
    expect_error({{"wetness", "wet"}}, "weather.wetness: expected a number in [0, 1]");
    expect_error(json::array(), "expected an object");
}

TEST_F(WeatherCodec, LenientScenesClampAndIgnoreUnknownFields) {
    ASSERT_OK(apply({{"wind_speed", 1000.0}, {"future_field", 3}}, /*strict=*/false));
    EXPECT_FLOAT_EQ(weather().wind_speed, 60.0F);
    // Wrong types are errors even in files.
    EXPECT_ERR(apply({{"precipitation_type", 1}}, /*strict=*/false));
}

TEST_F(WeatherCodec, OneWeatherPerScene) {
    ASSERT_OK(apply({{"wind_speed", 3.0}}));
    const entt::entity other = r.create();
    const auto second = s::apply_component(r, other, "weather", json::object(), true);
    ASSERT_FALSE(second.has_value());
    EXPECT_NE(second.error().message.find("already has its weather on entity"), std::string::npos);
    EXPECT_FALSE(r.all_of<w::WeatherState>(other));
    // Removing it frees the slot.
    EXPECT_TRUE(s::remove_component(r, e, "weather"));
    EXPECT_OK(s::apply_component(r, other, "weather", json::object(), true));
}

TEST(WeatherSchema, ListsEveryFieldWithRangeAndDefault) {
    const json schema = s::weather_schema();
    ASSERT_EQ(schema["fields"].size(), w::weather_fields().size());
    for (const json& f : schema["fields"]) {
        EXPECT_TRUE(f.contains("default")) << f.dump();
        EXPECT_TRUE(f.contains("hint")) << f.dump();
        if (f["type"] == "number") {
            EXPECT_LE(f["min"].get<float>(), f["default"].get<float>()) << f.dump();
            EXPECT_GE(f["max"].get<float>(), f["default"].get<float>()) << f.dump();
        }
    }
    EXPECT_EQ(schema["fields"][6]["name"], "precipitation_type");
    EXPECT_EQ(schema["fields"][6]["values"], json::array({"rain", "snow"}));
}

TEST(GpuWeatherSceneIo, WeatherRoundTripsThroughSceneFiles) {
    QUAT_REQUIRE_GPU(g);
    engine::asset::AssetManager assets;
    s::Scene scene;
    const entt::entity e = scene.create_entity("weather");
    w::WeatherState& state = scene.registry().emplace<w::WeatherState>(e);
    state.precipitation = 0.6F;
    state.precipitation_type = w::PrecipitationType::snow;
    state.snow = 0.45F;
    state.temperature_c = -3.0F;

    const auto path = std::filesystem::temp_directory_path() / "quat_weather_scene.json";
    ASSERT_OK(s::save_scene(scene, path));
    s::Scene loaded;
    ASSERT_OK(s::load_scene(loaded, path, g->allocator, g->transfer, assets));
    std::filesystem::remove(path);

    const entt::entity le = w::find_weather(loaded.registry());
    ASSERT_NE(le, entt::entity{entt::null});
    const w::WeatherState& back = loaded.registry().get<w::WeatherState>(le);
    EXPECT_EQ(s::encode_weather(back), s::encode_weather(state));
}

} // namespace
