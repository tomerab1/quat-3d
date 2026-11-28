// season.get / season.set (ROADMAP R25.1) and weather.set suspending the
// season's automatic weather.

#include <string>

#include <gtest/gtest.h>

#include "engine/astro/time.hpp"
#include "engine/scene/season.hpp"
#include "engine/scene/world_clock.hpp"
#include "engine/weather/climate.hpp"
#include "engine/weather/weather_state.hpp"
#include "remote/world_fixture.hpp"

namespace {

using quat::test::Json;
namespace code = engine::remote::rpc_code;
namespace a = engine::astro;
namespace s = engine::scene;
namespace w = engine::weather;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

class SeasonCommands : public quat::test::WorldCommands {
protected:
    void SetUp() override { register_commands(); }
    void paris_clock() {
        (void)ok("time.set", {{"datetime", "2026-06-21T12:00:00+02:00"},
                              {"latitude_deg", 48.8566},
                              {"longitude_deg", 2.3522},
                              {"time_scale", 0}});
    }
};

TEST_F(SeasonCommands, GetWithoutASeasonReportsTheDefaultDisabled) {
    const Json g = ok("season.get");
    EXPECT_EQ(g["in_scene"], false);
    EXPECT_EQ(g["enabled"], false);
    EXPECT_TRUE(g["entity"].is_null());
    EXPECT_EQ(g["mode"], "clock");
    EXPECT_NEAR(g["year_minutes"].get<double>(), 40.0, 1e-9);
    EXPECT_TRUE(g["weights"].contains("winter"));
}

TEST_F(SeasonCommands, ClockModeFollowsTheWorldClock) {
    paris_clock();
    const Json g = ok("season.set", Json::object());
    EXPECT_EQ(g["in_scene"], true);
    EXPECT_EQ(g["has_clock"], true);
    EXPECT_NEAR(g["year_phase"].get<double>(), 0.25, 0.001); // the June solstice
    EXPECT_EQ(g["year"], 2026);
    EXPECT_NEAR(g["day_length_h"].get<double>(), 16.2, 0.15);
    EXPECT_EQ(g["southern"], false);
    EXPECT_GT(g["climate"]["mean_temperature_c"].get<double>(), 12.0);
    EXPECT_TRUE(g["weather"]["kind"].is_string());
    EXPECT_TRUE(g["day_minutes"].is_null()); // the clock is frozen

    // A season name in clock mode moves the clock's date (time of day kept).
    const Json winter = ok("season.set", {{"season", "winter"}});
    EXPECT_EQ(winter["season"], "winter");
    EXPECT_NEAR(winter["year_phase"].get<double>(), 0.875, 0.003);
    EXPECT_LT(winter["day_length_h"].get<double>(), 10.5);
    const Json t = ok("time.get");
    EXPECT_EQ(t["datetime"].get<std::string>().substr(0, 7), "2027-02");
    EXPECT_EQ(t["datetime"].get<std::string>().substr(11, 5), "12:00");
}

TEST_F(SeasonCommands, CozyAcceleratedYear) {
    paris_clock();
    const Json g = ok("season.set", {{"mode", "independent"},
                                     {"year_minutes", 40},
                                     {"day_minutes", 12},
                                     {"drive_clock_date", true},
                                     {"season", "spring"}});
    EXPECT_EQ(g["mode"], "independent");
    EXPECT_NEAR(g["phase_rate"].get<double>(), 1.0 / 2400.0, 1e-15);
    EXPECT_NEAR(g["year_minutes"].get<double>(), 40.0, 1e-9);
    EXPECT_NEAR(g["day_minutes"].get<double>(), 12.0, 1e-9);
    EXPECT_NEAR(g["year_phase"].get<double>(), 0.125, 1e-12);
    // The clock's date follows the phase: mid-spring is early May.
    const s::WorldClock& c = reg().get<s::WorldClock>(s::find_world_clock(reg()));
    EXPECT_NEAR(c.time_scale, 120.0, 1e-9);
    EXPECT_EQ(a::civil_from_utc(c.utc, c.utc_offset_min).month, 5);
    // A minute of simulation is 1/40 of the year.
    for (int i = 0; i < 60; ++i) scene.tick(1.0F);
    EXPECT_NEAR(ok("season.get")["year_phase"].get<double>(), 0.125 + 1.0 / 40.0, 1e-9);
}

TEST_F(SeasonCommands, ManualSeasonWithoutAClock) {
    const Json g = ok("season.set", {{"mode", "manual"}, {"season", "autumn"}});
    EXPECT_EQ(g["season"], "autumn");
    EXPECT_EQ(g["has_clock"], false);
    EXPECT_TRUE(g["day_length_h"].is_null());
    scene.tick(100.0F);
    EXPECT_EQ(ok("season.get")["year_phase"], 0.625);
}

TEST_F(SeasonCommands, ValidatesEverythingBeforeChangingAnything) {
    paris_clock();
    (void)ok("season.set", {{"mode", "manual"}, {"year_phase", 0.3}});
    const auto bad = [&](const Json& p, const std::string& needle) {
        const std::string m = err("season.set", p, code::invalid_params);
        EXPECT_TRUE(contains(m, needle)) << p.dump() << " -> " << m;
    };
    bad({{"season", "monsoon"}}, "season: expected one of");
    bad({{"season", "winter"}, {"year_phase", 0.1}}, "either season or year_phase");
    bad({{"year_phase", 0.5}, {"colour", 1}}, "colour");
    bad({{"weather_preset", "hail"}}, "weather_preset: expected one of");
    bad({{"weather_preset", "rain"}, {"weather_auto", true}}, "suspends weather_auto");
    bad({{"real_calendar", true}, {"mode", "manual"}}, "mode must be \"clock\"");
    bad({{"real_calendar", true}, {"day_minutes", 10}}, "real_calendar");
    bad({{"spin_up_s", 99999}}, "spin_up_s");
    bad({{"climate", {{"cloud_cover", {2.0}}}}}, "climate.cloud_cover");
    bad({{"day_minutes", 0}}, "day_minutes");
    // Nothing changed.
    EXPECT_EQ(ok("season.get")["year_phase"], 0.3);

    // Without a clock: clock mode cannot take a phase, day_minutes has no clock.
    (void)ok("scene.destroy", {{"entity", entt::to_integral(s::find_world_clock(reg()))}});
    bad({{"mode", "clock"}, {"season", "summer"}}, "in clock mode the phase follows the world clock");
    bad({{"day_minutes", 12}}, "no world clock");
}

TEST_F(SeasonCommands, PresetsFollowTheSeasonAndSuspendAutoWeather) {
    (void)ok("season.set", {{"mode", "manual"}, {"season", "winter"}, {"climate", {{"temperature_c", {-6.0}}}}});
    const Json g = ok("season.set", {{"weather_preset", "showers"}});
    EXPECT_EQ(g["weather_auto"], false);
    EXPECT_EQ(g["climate"]["custom"], true);
    const Json wx = ok("weather.get")["weather"];
    EXPECT_EQ(wx["precipitation_type"], "snow") << "winter showers fall as snow";
    EXPECT_GT(wx["precipitation"].get<double>(), 0.0);

    // Back to the default (mild) climate in summer: showers are rain.
    (void)ok("season.set", {{"season", "summer"}, {"climate", nullptr}});
    (void)ok("season.set", {{"weather_preset", "showers"}});
    EXPECT_EQ(ok("weather.get")["weather"]["precipitation_type"], "rain");
    EXPECT_EQ(ok("season.get")["climate"]["custom"], false);

    // The automatic weather resumes, and a hand-set weather suspends it.
    EXPECT_EQ(ok("season.set", {{"weather_auto", true}})["weather_auto"], true);
    (void)ok("weather.set", {{"cloud_cover", 0.1}});
    EXPECT_EQ(ok("season.get")["weather_auto"], false);
    scene.tick(1.0F);
    EXPECT_NEAR(ok("weather.get")["weather"]["cloud_cover"].get<double>(), 0.1, 1e-6);
}

TEST_F(SeasonCommands, ClimateCurvesAndSpinUp) {
    const Json curves = ok("season.get", {{"climate", true}})["climate_curves"];
    EXPECT_EQ(curves["temperature_c"].size(), 12U);
    // A snowy winter, spun up for ten minutes: snow is lying.
    const Json g = ok("season.set", {{"mode", "manual"},
                                     {"season", "winter"},
                                     {"weather_seed", 2},
                                     {"climate",
                                      {{"temperature_c", {-8.0}},
                                       {"precipitation_chance", {1.0}},
                                       {"temperature_variability_c", 0.0}}},
                                     {"spin_up_s", 600}});
    EXPECT_EQ(g["year_phase"], 0.875);
    EXPECT_GT(ok("weather.get")["weather"]["snow"].get<double>(), 0.2);
}

TEST_F(SeasonCommands, RealCalendarRunsTheClockOnToday) {
    const Json g = ok("season.set", {{"real_calendar", true}});
    EXPECT_EQ(g["mode"], "clock");
    EXPECT_EQ(g["has_clock"], true);
    EXPECT_NEAR(g["day_minutes"].get<double>(), 1440.0, 1e-9);
    const s::WorldClock& c = reg().get<s::WorldClock>(s::find_world_clock(reg()));
    EXPECT_FALSE(c.fixed_day);
    EXPECT_GE(a::civil_from_utc(c.utc).year, 2026);
}

} // namespace
