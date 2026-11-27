// WorldClock (engine/scene/world_clock.hpp): advancing on the sim dt,
// driving the sun, the SkyState hand-off and the codec.

#include <cmath>
#include <cstring>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/astro/sky_state.hpp"
#include "engine/astro/time.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/world_clock.hpp"
#include "engine/scene/world_clock_json.hpp"

namespace {

namespace a = engine::astro;
namespace s = engine::scene;
using Json = nlohmann::json;

[[nodiscard]] a::UtcTime utc(int y, int mo, int d, int h = 0, int mi = 0) {
    return a::utc_from_civil({y, mo, d, h, mi, 0.0}, 0);
}

struct ClockScene {
    s::Scene     scene;
    entt::entity sun = entt::null;
    entt::entity clock = entt::null;

    explicit ClockScene(const s::WorldClock& c) {
        sun = scene.create_entity("sun");
        scene.registry().emplace<s::DirectionalLight>(sun, glm::normalize(glm::vec3(-0.4F, -1.0F, -0.3F)),
                                                      glm::vec3(1.0F), 3.0F);
        clock = scene.create_entity("world_clock");
        scene.registry().emplace<s::WorldClock>(clock, c);
    }
    [[nodiscard]] s::WorldClock&       wc() { return scene.registry().get<s::WorldClock>(clock); }
    [[nodiscard]] s::DirectionalLight& light() { return scene.registry().get<s::DirectionalLight>(sun); }
};

[[nodiscard]] s::WorldClock paris(a::UtcTime t) {
    s::WorldClock c;
    c.utc = t;
    c.latitude_deg = 48.8566;
    c.longitude_deg = 2.3522;
    c.utc_offset_min = 120;
    return c;
}

TEST(WorldClock, AdvancesOnTheSimulationDtTimesItsScale) {
    s::WorldClock c = paris(utc(2026, 6, 21, 4));
    c.time_scale = 60.0;
    ClockScene x(c);
    ClockScene y(c);
    for (int i = 0; i < 600; ++i) {
        x.scene.tick(1.0F / 60.0F);
        y.scene.tick(1.0F / 60.0F);
    }
    // 10 s of simulation at 60x = 600 clock seconds, identical in both runs.
    EXPECT_EQ(x.wc().utc, y.wc().utc);
    EXPECT_NEAR(static_cast<double>(x.wc().utc.ns - utc(2026, 6, 21, 4).ns) * 1e-9, 600.0, 1e-3);
    // A paused simulation (dt 0) and a zero time scale both freeze it.
    const a::UtcTime before = x.wc().utc;
    x.scene.tick(0.0F);
    EXPECT_EQ(x.wc().utc, before);
    x.wc().time_scale = 0.0;
    x.scene.tick(0.5F);
    EXPECT_EQ(x.wc().utc, before);
}

TEST(WorldClock, DrivesTheSunThroughTheDay) {
    // Paris, 2026 June solstice: solar noon 13:52 local (11:52 UTC), sunset
    // 21:58 local.
    ClockScene x(paris(utc(2026, 6, 21, 11, 52)));
    x.wc().sun_intensity = 5.0F;
    x.scene.tick(0.0F);
    const auto* sky = x.scene.registry().try_get<a::SkyState>(x.clock);
    ASSERT_NE(sky, nullptr);
    EXPECT_NEAR(sky->sun_elevation_deg, 64.6F, 0.1F);
    EXPECT_NEAR(glm::distance(x.light().direction, -sky->sun_direction), 0.0F, 1e-6F);
    EXPECT_FLOAT_EQ(x.light().intensity, 5.0F);
    EXPECT_LT(x.light().direction.y, -0.8F); // high sun shines down
    EXPECT_LT(x.light().direction.z, 0.0F);  // from the south (+Z) toward −Z

    x.wc().utc = utc(2026, 6, 21, 22); // midnight local: the Sun is down
    x.scene.tick(0.0F);
    EXPECT_FLOAT_EQ(x.light().intensity, 0.0F);
    EXPECT_GT(x.light().direction.y, 0.0F); // it would shine from below
}

TEST(WorldClock, DisabledClockLeavesTheSceneExactlyAsItWas) {
    s::WorldClock c = paris(utc(2026, 6, 21, 4));
    c.enabled = false;
    ClockScene              x(c);
    const s::DirectionalLight before = x.light();
    for (int i = 0; i < 10; ++i) x.scene.tick(0.1F);
    EXPECT_EQ(std::memcmp(&before, &x.light(), sizeof before), 0);
    EXPECT_EQ(x.wc().utc, utc(2026, 6, 21, 4));
    EXPECT_FALSE(x.scene.registry().all_of<a::SkyState>(x.clock));

    // drive_sun off: the sky is derived, the light is not touched.
    x.wc().enabled = true;
    x.wc().drive_sun = false;
    x.scene.tick(0.1F);
    EXPECT_TRUE(x.scene.registry().all_of<a::SkyState>(x.clock));
    EXPECT_EQ(std::memcmp(&before, &x.light(), sizeof before), 0);

    // Disabling again drops the stale sky.
    x.wc().enabled = false;
    x.scene.tick(0.1F);
    EXPECT_FALSE(x.scene.registry().all_of<a::SkyState>(x.clock));
}

TEST(WorldClock, FixedDayWrapsAtLocalMidnight) {
    s::WorldClock c = paris(utc(2026, 6, 21, 21, 30)); // 23:30 local
    c.fixed_day = true;
    // +1 h -> 00:30 local of the SAME day (2026-06-20 22:30 UTC).
    EXPECT_EQ(s::advance_clock_time(c, 3600.0), utc(2026, 6, 20, 22, 30));
    // −24 h is a no-op; +25 h is +1 h.
    EXPECT_EQ(s::advance_clock_time(c, -86400.0), c.utc);
    EXPECT_EQ(s::advance_clock_time(c, 25.0 * 3600.0), utc(2026, 6, 20, 22, 30));
    c.fixed_day = false;
    EXPECT_EQ(s::advance_clock_time(c, 3600.0), utc(2026, 6, 21, 22, 30));
    // Clamped to the supported years.
    c.utc = utc(2200, 12, 31, 23);
    EXPECT_TRUE(a::in_supported_range(s::advance_clock_time(c, 1.0e9)));
}

TEST(WorldClock, CodecRoundTripsAndValidates) {
    s::WorldClock c = paris(a::add_seconds(utc(2026, 6, 21, 3, 15), 0.25));
    c.time_scale = 120.0;
    c.fixed_day = true;
    c.north_offset_deg = 30.0;
    c.sun_intensity = 4.5F;
    const Json j = s::encode_world_clock(c);
    EXPECT_EQ(j["datetime"], "2026-06-21T05:15:00.25+02:00");
    EXPECT_EQ(j["utc_offset_h"], 2.0);

    entt::registry     r;
    const entt::entity e = r.create();
    ASSERT_TRUE(s::apply_component(r, e, "world_clock", j, /*strict=*/true).has_value());
    const s::WorldClock& back = r.get<s::WorldClock>(e);
    EXPECT_EQ(back.utc, c.utc);
    EXPECT_EQ(back.utc_offset_min, 120);
    EXPECT_DOUBLE_EQ(back.latitude_deg, c.latitude_deg);
    EXPECT_DOUBLE_EQ(back.longitude_deg, c.longitude_deg);
    EXPECT_DOUBLE_EQ(back.time_scale, 120.0);
    EXPECT_TRUE(back.fixed_day);
    EXPECT_DOUBLE_EQ(back.north_offset_deg, 30.0);
    EXPECT_FLOAT_EQ(back.sun_intensity, 4.5F);
    EXPECT_EQ(s::encode_components(r, e)["world_clock"], j);

    // A datetime without a zone is local time at the clock's offset.
    ASSERT_TRUE(s::apply_component(r, e, "world_clock", Json{{"datetime", "2026-01-01T12:00"}}, true));
    EXPECT_EQ(r.get<s::WorldClock>(e).utc, utc(2026, 1, 1, 10));
    // A zone designator becomes the display zone.
    ASSERT_TRUE(s::apply_component(r, e, "world_clock", Json{{"datetime", "2026-01-01T12:00Z"}}, true));
    EXPECT_EQ(r.get<s::WorldClock>(e).utc_offset_min, 0);

    const auto error = [&](const Json& v) {
        auto ok = s::apply_component(r, e, "world_clock", v, true);
        return ok ? std::string("<accepted>") : ok.error().message;
    };
    EXPECT_NE(error({{"datetime", "2026-02-30T00:00"}}).find("world_clock.datetime: day 30"), std::string::npos);
    EXPECT_NE(error({{"latitude_deg", 91}}).find("world_clock.latitude_deg"), std::string::npos);
    EXPECT_NE(error({{"time_scale", -1}}).find("world_clock.time_scale"), std::string::npos);
    EXPECT_NE(error({{"timezone", 2}}).find("unknown field"), std::string::npos);
    EXPECT_NE(error({{"utc_offset_h", 15}}).find("utc_offset_h"), std::string::npos);

    // One clock per scene.
    const entt::entity other = r.create();
    EXPECT_NE(s::apply_component(r, other, "world_clock", Json::object(), true).error().message.find(
                  "already has its clock"),
              std::string::npos);
    EXPECT_TRUE(s::remove_component(r, e, "world_clock"));
    EXPECT_TRUE(s::apply_component(r, other, "world_clock", Json::object(), true).has_value());
}

TEST(WorldClock, AlmanacListsTheLocalDay) {
    const s::DayAlmanac d = s::day_almanac(paris(utc(2026, 6, 21, 10)));
    EXPECT_EQ(d.day_start, utc(2026, 6, 20, 22));
    ASSERT_TRUE(d.sunrise && d.sunset && d.solar_noon && d.civil_dawn && d.civil_dusk);
    // Published almanacs for Paris that day: sunrise 05:47, sunset 21:58 local.
    EXPECT_NEAR(static_cast<double>(d.sunrise->ns - utc(2026, 6, 21, 3, 47).ns) * 1e-9, 0.0, 60.0);
    EXPECT_NEAR(static_cast<double>(d.sunset->ns - utc(2026, 6, 21, 19, 58).ns) * 1e-9, 0.0, 60.0);
    EXPECT_LT(*d.civil_dawn, *d.sunrise);
    EXPECT_GT(*d.civil_dusk, *d.sunset);
    EXPECT_FALSE(d.polar_day || d.polar_night);
}

} // namespace
