// R22.1: the Lua `time.clock` / `time.set_clock` / `time.skip` bindings of
// the world clock (the same surface as the remote time.* commands).

#include "engine/astro/time.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/world_clock.hpp"
#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

using namespace engine::scene;

TEST_F(ScriptTest, WorldClockFromLua) {
    write("clock.lua", R"(
        function on_start(self)
            print("before", time.clock().in_scene)
            local c = time.set_clock({ datetime = "2026-06-21T13:52:00+02:00",
                                       latitude_deg = 48.8566, longitude_deg = 2.3522,
                                       time_scale = 0 })
            print("set", c.in_scene, c.datetime, string.format("%.0f", c.sun.azimuth_deg))
            print("sunrise", string.sub(c.sun.sunrise, 12, 16), c.moon.phase)
            print("dir", c.sun.direction.y > 0.8)
            c = time.skip(8)
            print("skip", c.datetime, c.sun.elevation_deg < 0)
            c = time.set_clock({ local_time = "06:30" })
            print("local", c.datetime)
            local ok, err = pcall(time.set_clock, { latitude_deg = 120 })
            print("bad", ok, err)
            ok, err = pcall(time.set_clock, { timezone = 2 })
            print("unknown", ok, err)
            ok, err = pcall(time.skip, 1e9)
            print("far", ok, err)
        end
    )");
    (void)scripted("clocker", "clock.lua");
    start();
    frames();
    const auto log = script_log();
    EXPECT_TRUE(has_line(log, "before\tfalse")) << ::testing::PrintToString(log);
    EXPECT_TRUE(has_line(log, "set\ttrue\t2026-06-21T13:52:00+02:00\t180"));
    EXPECT_TRUE(has_line(log, "sunrise\t05:46\tfirst quarter"));
    EXPECT_TRUE(has_line(log, "dir\ttrue"));
    EXPECT_TRUE(has_line(log, "skip\t2026-06-21T21:52:00+02:00\tfalse"));
    EXPECT_TRUE(has_line(log, "local\t2026-06-21T06:30:00+02:00"));
    EXPECT_TRUE(has_line(log, "bad\tfalse\tset_clock: latitude_deg: expected a number in [-90, 90]"));
    EXPECT_TRUE(has_line(log, "timezone: unknown field"));
    EXPECT_TRUE(has_line(log, "far\tfalse\tskip: hours: expected"));
    // The clock exists, frozen at the last valid setting, and drives a sun.
    const entt::entity clock = find_world_clock(scene.registry());
    ASSERT_NE(clock, entt::entity{entt::null});
    EXPECT_EQ(scene.registry().get<WorldClock>(clock).utc,
              engine::astro::utc_from_civil({2026, 6, 21, 6, 30, 0.0}, 120));
    EXPECT_EQ(scene.registry().view<DirectionalLight>().size(), 1U);
}

} // namespace
} // namespace quat::test
