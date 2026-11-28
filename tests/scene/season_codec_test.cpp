// Season and climate component codecs (R25.1): scene files and scene.*.

#include <string>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/season.hpp"
#include "engine/scene/season_json.hpp"
#include "engine/weather/climate.hpp"
#include "support/expect_ok.hpp"

namespace {

using json = nlohmann::json;
namespace s = engine::scene;
namespace w = engine::weather;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

class SeasonCodec : public ::testing::Test {
protected:
    entt::registry r;
    entt::entity   e = r.create();
};

TEST_F(SeasonCodec, SeasonRoundTripsEveryField) {
    const json authored = {{"enabled", true},          {"mode", "independent"},
                           {"year_phase", 0.62},       {"year", 2031},
                           {"phase_rate", 1.0 / 2400}, {"drive_clock_date", true},
                           {"weather_auto", false},    {"weather_seed", 4000000000U},
                           {"weather_clock_time", false}};
    ASSERT_OK(s::apply_component(r, e, "season", authored, true));
    const s::SeasonState& st = r.get<s::SeasonState>(e);
    EXPECT_EQ(st.mode, s::SeasonMode::independent);
    EXPECT_EQ(st.year, 2031);
    EXPECT_EQ(st.weather_seed, 4000000000U);
    const json encoded = s::encode_components(r, e)["season"];
    EXPECT_EQ(encoded, authored);
    // Re-decoding what was encoded changes nothing.
    const s::SeasonState before = st;
    ASSERT_OK(s::apply_component(r, e, "season", encoded, true));
    EXPECT_EQ(r.get<s::SeasonState>(e), before);
}

TEST_F(SeasonCodec, SeasonRejectsBadFieldsAndASecondSeason) {
    const auto message = [&](const json& v) {
        auto ok = s::apply_component(r, e, "season", v, true);
        return ok ? std::string{} : ok.error().message;
    };
    EXPECT_TRUE(contains(message({{"mode", "lunar"}}), "mode"));
    EXPECT_TRUE(contains(message({{"year_phase", 1.5}}), "year_phase"));
    EXPECT_TRUE(contains(message({{"year", 2500}}), "year"));
    EXPECT_TRUE(contains(message({{"phase_rate", -1}}), "phase_rate"));
    EXPECT_TRUE(contains(message({{"phase", 0.3}}), "phase")); // unknown field
    // year_phase 1 is next year's 0.
    ASSERT_OK(s::apply_component(r, e, "season", {{"year_phase", 1.0}}, true));
    EXPECT_EQ(r.get<s::SeasonState>(e).year_phase, 0.0);

    const entt::entity other = r.create();
    auto second = s::apply_component(r, other, "season", json::object(), true);
    ASSERT_FALSE(second);
    EXPECT_TRUE(contains(second.error().message, "already has its season"));
    // Listed and removable like any component.
    const json names = s::encode_components(r, e);
    EXPECT_TRUE(names.contains("season"));
    EXPECT_TRUE(s::remove_component(r, e, "season"));
    EXPECT_FALSE(r.all_of<s::SeasonState>(e));
}

TEST_F(SeasonCodec, ClimateCurvesRoundTripAndAreValidated) {
    // Curves are floats: values exact in binary round-trip bit for bit.
    const json authored = {{"temperature_c", {-5.0, 5.0, 20.0, 5.0}},
                           {"diurnal_range_c", {8.0}},
                           {"precipitation_chance", {0.25, 0.75}},
                           {"cloud_cover", {0.5}},
                           {"wind_speed", {3.0, 6.0, 3.0}},
                           {"temperature_variability_c", 2.0},
                           {"weather_period_days", 5.0},
                           {"transition", 0.5}};
    ASSERT_OK(s::apply_component(r, e, "climate", authored, true));
    const w::Climate& c = r.get<w::Climate>(e);
    EXPECT_EQ(c.temperature_c.size(), 4U);
    EXPECT_EQ(c.weather_period_days, 5.0F);
    EXPECT_EQ(s::encode_components(r, e)["climate"], authored);

    const auto message = [&](const json& v) {
        auto ok = s::apply_component(r, e, "climate", v, true);
        return ok ? std::string{} : ok.error().message;
    };
    EXPECT_TRUE(contains(message({{"cloud_cover", {0.5, 1.5}}}), "cloud_cover: expected an array of 1..48"));
    EXPECT_TRUE(contains(message({{"temperature_c", json::array()}}), "temperature_c"));
    EXPECT_TRUE(contains(message({{"wind_speed", std::vector<double>(49, 3.0)}}), "wind_speed"));
    EXPECT_TRUE(contains(message({{"wind_speed", {"windy"}}}), "wind_speed"));
    EXPECT_TRUE(contains(message({{"weather_period_days", 0.0}}), "weather_period_days"));
    // A failed edit left the climate as it was.
    EXPECT_EQ(r.get<w::Climate>(e).temperature_c.size(), 4U);
}

} // namespace
