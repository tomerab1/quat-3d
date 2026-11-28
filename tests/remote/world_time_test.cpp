// time.get / time.set / time.skip (ROADMAP R22.1), environment.set's
// refusal to move a clock-driven sun, and the HDRI's sun taking the light
// from the clock (issue #121).

#include <cmath>
#include <expected>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/hdri_sun.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/scene/sun_source.hpp"
#include "engine/scene/world_clock.hpp"
#include "remote/world_fixture.hpp"
#include "support/temp_dir.hpp"

namespace {

using quat::test::Json;
namespace code = engine::remote::rpc_code;
namespace s = engine::scene;
namespace r = engine::renderer;

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
    // The local year is in range but the UTC instant is not.
    EXPECT_TRUE(contains(err("time.set", {{"datetime", "2200-12-31T23:00-05:00"}}, code::invalid_params),
                         "outside the supported years"));
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
    Json env = ok("environment.set", {{"sun", {{"color", "#ffcc88"}}}});
    EXPECT_EQ(env["sun"]["driven_by_clock"], true);
    // The two azimuths name different angles of the same light: the Sun's
    // compass bearing (time.get) and the travel direction's world angle
    // atan2(d.z, d.x) (environment.get). Near solar noon in Paris the Sun is
    // south (compass ~180) and the light travels north (-Z): about -90.
    const Json t = ok("time.set", {{"local_time", "13:52"}});
    EXPECT_NEAR(t["sun"]["azimuth_deg"].get<double>(), 180.0, 1.0);
    EXPECT_NEAR(ok("environment.get")["sun"]["azimuth_deg"].get<double>(), -90.0, 1.0);
    // Once the clock lets go, the angles are free again.
    (void)ok("time.set", {{"drive_sun", false}});
    env = ok("environment.set", {{"sun", {{"elevation_deg", 10}}}});
    EXPECT_NEAR(env["sun"]["elevation_deg"].get<double>(), 10.0, 1e-3);
    EXPECT_EQ(env["sun"]["driven_by_clock"], false);
    EXPECT_EQ(env["sun"]["source"], "manual");
}

// A loaded HDRI (services.hdri_info, faked) under a running world clock.
class TimeWithHdri : public quat::test::WorldCommands {
protected:
    r::RenderSettings        settings;
    engine::remote::HdriInfo hdri;
    quat::test::TempDir      root{"time_hdri_root"};
    std::vector<std::string> loads; // services.set_hdri calls
    void SetUp() override {
        services.render_settings = &settings;
        services.hdri_info = [this] { return hdri; };
        services.project_root = [this] { return root.path(); };
        services.set_hdri = [this](const std::string& relative)
            -> std::expected<void, engine::core::Error> {
            loads.push_back(relative);
            hdri.path = relative;
            hdri.loaded = !relative.empty();
            return {};
        };
        register_commands();
        hdri.path = "hdri-cache/field.hdr";
        hdri.loaded = true;
        hdri.width = 2048;
        hdri.height = 1024;
        hdri.sun.found = true;
        hdri.sun.direction = glm::normalize(glm::vec3(0.5F, 0.6F, -0.4F));
        hdri.sun.irradiance = glm::vec3(30.0F, 24.0F, 15.0F);
    }
    // The sun the frame is lit with: FrameRenderer::gather's decision.
    [[nodiscard]] s::SceneSun lit() {
        return s::resolve_sun(reg(), settings.environment, hdri.loaded ? &hdri.sun : nullptr);
    }
    // environment.get's sun is the lit one.
    void expect_reported_as_lit(const Json& sun) {
        const s::SceneSun l = lit();
        ASSERT_TRUE(l.light.has_value());
        const glm::vec3 d = glm::normalize(l.light->direction);
        EXPECT_EQ(sun["source"], std::string(s::sun_source_name(l.source)));
        EXPECT_EQ(sun["driven_by_clock"], l.source == s::SunSource::clock);
        EXPECT_NEAR(sun["azimuth_deg"].get<float>(), glm::degrees(std::atan2(d.z, d.x)), 1e-3F);
        EXPECT_NEAR(sun["elevation_deg"].get<float>(), glm::degrees(std::asin(-d.y)), 1e-3F);
        EXPECT_NEAR(sun["intensity"].get<float>(), l.light->intensity, 1e-4F);
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(sun["color"][i].get<float>(), l.light->color[i], 1e-5F);
        }
    }
};

TEST_F(TimeWithHdri, TheHdriSunOwnsTheLightAndEnvironmentGetSaysSo) {
    (void)ok("time.set", {{"datetime", "2026-06-21T12:00:00+02:00"}, {"time_scale", 0}});
    const Json env = ok("environment.set", {{"sky", "hdri"}, {"hdri", {{"rotation_deg", 30}}}});
    ASSERT_TRUE(env["sun"].is_object());
    EXPECT_EQ(env["sun"]["source"], "hdri");
    EXPECT_EQ(env["sun"]["driven_by_clock"], false);
    expect_reported_as_lit(env["sun"]);
    // It is the panorama's sun as the environment places it.
    const r::HdriSunLight expected = r::hdri_sun_light(hdri.sun, 30.0F, 1.0F);
    EXPECT_NEAR(env["sun"]["elevation_deg"].get<float>(),
                glm::degrees(std::asin(expected.direction_to_sun.y)), 1e-3F);
    EXPECT_NEAR(env["sun"]["intensity"].get<float>(), expected.intensity, 1e-4F);

    // The clock keeps running (its sky still advances for stars and moon)
    // but no longer reaches the light.
    const Json before = ok("time.get");
    (void)ok("time.skip", {{"hours", 3}});
    const Json after = ok("time.get");
    EXPECT_NE(before["sun"]["elevation_deg"], after["sun"]["elevation_deg"]);
    EXPECT_NE(before["moon"]["elevation_deg"], after["moon"]["elevation_deg"]);
    const Json still = ok("environment.get");
    EXPECT_EQ(still["sun"], env["sun"]);
    expect_reported_as_lit(still["sun"]);

    // Sun edits name the HDRI, colour included, and change nothing.
    for (const std::string key : {"elevation_deg", "azimuth_deg", "intensity"}) {
        const std::string msg = err("environment.set", {{"sun", {{key, 10}}}}, code::invalid_params);
        EXPECT_TRUE(contains(msg, "sun." + key + ": the HDRI's own sun lights the scene")) << msg;
    }
    EXPECT_TRUE(contains(err("environment.set", {{"sun", {{"color", "#ffffff"}}}}, code::invalid_params),
                         "hdri.rotation_deg"));
    // Turning hdri.sun off in the same request hands the sun to the clock,
    // which still refuses the angles; the refusal changes nothing.
    EXPECT_TRUE(contains(err("environment.set", {{"hdri", {{"sun", false}}}, {"sun", {{"elevation_deg", 10}}}},
                             code::invalid_params),
                         "the world clock drives the sun"));
    EXPECT_TRUE(settings.environment.hdri_sun);
    EXPECT_EQ(ok("environment.get")["sun"], env["sun"]);
}

// The owner is predicted from the request: a request that loads an HDRI
// (with its sun on) and moves the sun is refused before the file loads.
TEST_F(TimeWithHdri, ARequestLoadingAnHdriCannotAlsoMoveTheSun) {
    hdri = {}; // nothing loaded yet
    (void)ok("time.set", {{"datetime", "2026-06-21T12:00:00+02:00"}, {"time_scale", 0}});
    const Json before = ok("environment.get");
    EXPECT_EQ(before["sun"]["source"], "clock");
    const std::string msg = err("environment.set",
                                {{"sky", "hdri"}, {"hdri", {{"path", "x.hdr"}}},
                                 {"sun", {{"elevation_deg", 10}}}},
                                code::invalid_params);
    EXPECT_TRUE(contains(msg, "sun.elevation_deg: the HDRI's own sun lights the scene")) << msg;
    EXPECT_TRUE(loads.empty());
    EXPECT_FALSE(hdri.loaded);
    EXPECT_EQ(settings.environment.source, static_cast<int>(r::SkySource::procedural));
    EXPECT_EQ(ok("environment.get"), before);
}

TEST_F(TimeWithHdri, TheClockTakesTheSunBackWhenTheHdriLetsGo) {
    (void)ok("time.set", {{"datetime", "2026-06-21T12:00:00+02:00"}, {"time_scale", 0}});
    (void)ok("environment.set", {{"sky", "hdri"}});
    EXPECT_EQ(ok("environment.get")["sun"]["source"], "hdri");

    // hdri.sun off: the HDRI lights as captured and the clock's light stays.
    Json env = ok("environment.set", {{"hdri", {{"sun", false}}}});
    EXPECT_EQ(env["sun"]["source"], "clock");
    EXPECT_EQ(env["sun"]["driven_by_clock"], true);
    expect_reported_as_lit(env["sun"]);
    const entt::entity light = *reg().view<s::DirectionalLight>().begin();
    EXPECT_NEAR(env["sun"]["intensity"].get<float>(), reg().get<s::DirectionalLight>(light).intensity,
                1e-5F);

    // Back on, then the panorama unloaded: the clock again.
    (void)ok("environment.set", {{"hdri", {{"sun", true}}}});
    EXPECT_EQ(ok("environment.get")["sun"]["source"], "hdri");
    hdri.loaded = false;
    env = ok("environment.get");
    EXPECT_EQ(env["sun"]["source"], "clock");
    expect_reported_as_lit(env["sun"]);

    // Without the clock driving it, the sun is the scene's own again.
    hdri.loaded = true;
    (void)ok("time.set", {{"drive_sun", false}});
    EXPECT_EQ(ok("environment.get")["sun"]["source"], "hdri");
    (void)ok("environment.set", {{"sky", "procedural"}});
    env = ok("environment.set", {{"sun", {{"elevation_deg", 20}}}});
    EXPECT_EQ(env["sun"]["source"], "manual");
    EXPECT_NEAR(env["sun"]["elevation_deg"].get<double>(), 20.0, 1e-3);
    expect_reported_as_lit(env["sun"]);
}

TEST_F(TimeCommands, TheClockIsASceneComponent) {
    // scene.create can add it like any component, and only once.
    const Json created = ok("scene.create", {{"name", "clock"},
                                             {"components", {{"world_clock", {{"latitude_deg", -33.86}}}}}});
    EXPECT_EQ(ok("time.get")["latitude_deg"], -33.86);
    EXPECT_TRUE(contains(err("scene.create", {{"components", {{"world_clock", Json::object()}}}},
                             code::failed),
                         "already has its clock"));
    (void)created;
}

} // namespace
