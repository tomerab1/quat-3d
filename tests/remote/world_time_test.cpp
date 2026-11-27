// time.get / time.set / time.skip (ROADMAP R22.1) and environment.set's
// refusal to move a clock-driven sun.

#include <string>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/scene/world_clock.hpp"
#include "remote/world_fixture.hpp"

namespace {

using quat::test::Json;
namespace code = engine::remote::rpc_code;
namespace s = engine::scene;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

class TimeCommands : public quat::test::WorldCommands {
protected:
    void SetUp() override { register_commands(); }
};

TEST_F(TimeCommands, GetWithoutAClockReportsTheDefaultDisabled) {
    const Json t = ok("time.get");
    EXPECT_EQ(t["in_scene"], false);
    EXPECT_EQ(t["enabled"], false);
    EXPECT_TRUE(t["entity"].is_null());
    EXPECT_TRUE(t["sun"]["sunrise"].is_string());
    EXPECT_TRUE(t["moon"]["phase"].is_string());
}

TEST_F(TimeCommands, SetCreatesTheClockAndASunToDrive) {
    // Paris, 2026 June solstice, 21:30 local: the Sun low in the north-west.
    const Json t = ok("time.set", {{"datetime", "2026-06-21T21:30:00+02:00"},
                                   {"latitude_deg", 48.8566},
                                   {"longitude_deg", 2.3522}});
    EXPECT_EQ(t["in_scene"], true);
    EXPECT_EQ(t["enabled"], true);
    EXPECT_EQ(t["datetime"], "2026-06-21T21:30:00+02:00");
    EXPECT_EQ(t["utc"], "2026-06-21T19:30:00Z");
    EXPECT_EQ(t["utc_offset_h"], 2.0);
    EXPECT_GT(t["sun"]["elevation_deg"].get<double>(), 0.0);
    EXPECT_LT(t["sun"]["elevation_deg"].get<double>(), 6.0);
    EXPECT_NEAR(t["sun"]["azimuth_deg"].get<double>(), 300.0, 10.0);
    EXPECT_EQ(t["sun"]["sunrise"].get<std::string>().substr(0, 16), "2026-06-21T05:46");
    EXPECT_EQ(t["sun"]["sunset"].get<std::string>().substr(0, 16), "2026-06-21T21:57");
    EXPECT_NEAR(t["sun"]["day_length_h"].get<double>(), 16.18, 0.02);
    EXPECT_TRUE(t["sun"]["polar"].is_null());

    // The scene had no light: one was created and points from the Sun.
    const entt::entity clock = s::find_world_clock(reg());
    ASSERT_NE(clock, entt::entity{entt::null});
    int lights = 0;
    for (auto [e, l] : reg().view<s::DirectionalLight>().each()) {
        ++lights;
        const Json& d = t["sun"]["direction"];
        EXPECT_NEAR(l.direction.x, -d[0].get<float>(), 1e-6F);
        EXPECT_NEAR(l.direction.y, -d[1].get<float>(), 1e-6F);
        EXPECT_GT(l.intensity, 0.0F);
    }
    EXPECT_EQ(lights, 1);
}

TEST_F(TimeCommands, LocalTimeKeepsTheDateAndSkipMovesTheClock) {
    (void)ok("time.set", {{"datetime", "2026-03-20T08:00:00+01:00"}, {"time_scale", 0}});
    Json t = ok("time.set", {{"local_time", "18:45"}});
    EXPECT_EQ(t["datetime"], "2026-03-20T18:45:00+01:00");
    EXPECT_EQ(t["time_scale"], 0.0);
    t = ok("time.skip", {{"hours", 6.25}});
    EXPECT_EQ(t["datetime"], "2026-03-21T01:00:00+01:00");
    t = ok("time.skip", {{"hours", -48}});
    EXPECT_EQ(t["datetime"], "2026-03-19T01:00:00+01:00");
    // Under fixed_day the skip wraps within the day.
    t = ok("time.set", {{"fixed_day", true}});
    t = ok("time.skip", {{"hours", 25}});
    EXPECT_EQ(t["datetime"], "2026-03-19T02:00:00+01:00");
}

TEST_F(TimeCommands, RejectsBadParametersPrecisely) {
    EXPECT_TRUE(contains(err("time.set", {{"datetime", "2026-02-30T10:00"}}, code::invalid_params),
                         "datetime: day 30 out of range 1..28"));
    EXPECT_TRUE(contains(err("time.set", {{"datetime", 2026}}, code::invalid_params), "datetime: expected a string"));
    EXPECT_TRUE(contains(err("time.set", {{"latitude_deg", 120}}, code::invalid_params),
                         "latitude_deg: expected a number in [-90, 90]"));
    EXPECT_TRUE(contains(err("time.set", {{"local_time", "7pm"}}, code::invalid_params), "local_time:"));
    EXPECT_TRUE(contains(err("time.set", {{"speed", 10}}, code::invalid_params), "unknown field"));
    EXPECT_TRUE(contains(err("time.set", {{"datetime", "1500-01-01"}}, code::invalid_params), "1800..2200"));
    // Nothing was created by the failed calls.
    EXPECT_EQ(s::find_world_clock(reg()), entt::entity{entt::null});

    EXPECT_TRUE(contains(err("time.skip", {{"hours", 1}}, code::not_found), "time.set first"));
    (void)ok("time.set", Json::object());
    EXPECT_TRUE(contains(err("time.skip", Json::object(), code::invalid_params), "hours: required"));
    EXPECT_TRUE(contains(err("time.skip", {{"hours", 1e9}}, code::invalid_params), "hours"));
    EXPECT_TRUE(contains(err("time.get", {{"x", 1}}, code::invalid_params), "unknown field"));
}

TEST_F(TimeCommands, EnvironmentSetCannotMoveADrivenSun) {
    (void)ok("time.set", {{"datetime", "2026-06-21T12:00:00+02:00"}});
    EXPECT_TRUE(contains(err("environment.set", {{"sun", {{"elevation_deg", 10}}}}, code::invalid_params),
                         "the world clock drives the sun"));
    // Colour stays the user's.
    (void)ok("environment.set", {{"sun", {{"color", "#ffcc88"}}}});
    // Once the clock lets go, the angles are free again.
    (void)ok("time.set", {{"drive_sun", false}});
    const Json env = ok("environment.set", {{"sun", {{"elevation_deg", 10}}}});
    EXPECT_NEAR(env["sun"]["elevation_deg"].get<double>(), 10.0, 1e-3);
}

TEST_F(TimeCommands, TheClockIsASceneComponent) {
    // scene.create can add it like any component, and only once.
    const Json created = ok("scene.create", {{"name", "clock"},
                                             {"components", {{"world_clock", {{"latitude_deg", -33.86}}}}}});
    EXPECT_EQ(ok("time.get")["latitude_deg"], -33.86);
    EXPECT_TRUE(contains(err("scene.create", {{"components", {{"world_clock", Json::object()}}}},
                             code::invalid_params),
                         "already has its clock"));
    (void)created;
}

} // namespace
