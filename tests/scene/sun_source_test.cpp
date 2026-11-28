// The sun's one owner (engine/scene/sun_source.hpp, issue #121): the HDRI's
// own sun outranks the world clock, which outranks the scene's light, and
// the clock keeps running under an HDRI without reaching the frame.

#include <algorithm>
#include <cmath>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/astro/sky_state.hpp"
#include "engine/astro/time.hpp"
#include "engine/renderer/hdri_sun.hpp"
#include "engine/renderer/reflection_settings.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/sun_source.hpp"
#include "engine/scene/world_clock.hpp"

namespace {

namespace r = engine::renderer;
namespace s = engine::scene;

// A panorama sun: high in the south-east of the panorama, warm.
[[nodiscard]] r::HdriSun panorama_sun() {
    r::HdriSun sun;
    sun.found = true;
    sun.direction = glm::normalize(glm::vec3(0.5F, 0.6F, -0.4F));
    sun.irradiance = glm::vec3(30.0F, 24.0F, 15.0F);
    return sun;
}

[[nodiscard]] r::EnvironmentSettings hdri_sky(bool hdri_sun = true) {
    r::EnvironmentSettings e;
    e.source = static_cast<int>(r::SkySource::hdri);
    e.hdri_sun = hdri_sun;
    return e;
}

struct World {
    entt::registry reg;
    entt::entity   light = entt::null;
    entt::entity   clock = entt::null;

    explicit World(bool with_clock = true) {
        light = reg.create();
        reg.emplace<s::DirectionalLight>(light, glm::normalize(glm::vec3(0.3F, -1.0F, 0.2F)),
                                         glm::vec3(1.0F, 0.9F, 0.8F), 2.0F);
        if (with_clock) {
            clock = reg.create();
            s::WorldClock c;
            c.utc = engine::astro::utc_from_civil({2026, 6, 21, 10, 0, 0.0}, 0); // Paris noon
            c.time_scale = 3600.0;                                                // an hour a second
            reg.emplace<s::WorldClock>(clock, c);
            s::apply_world_clock(reg);
        }
    }
    [[nodiscard]] s::WorldClock&       wc() { return reg.get<s::WorldClock>(clock); }
    [[nodiscard]] s::DirectionalLight& dl() { return reg.get<s::DirectionalLight>(light); }
};

void expect_light_eq(const s::DirectionalLight& a, const s::DirectionalLight& b) {
    EXPECT_NEAR(glm::length(a.direction - b.direction), 0.0F, 1e-5F);
    EXPECT_NEAR(glm::length(a.color - b.color), 0.0F, 1e-5F);
    EXPECT_NEAR(a.intensity, b.intensity, 1e-5F * std::max(1.0F, b.intensity));
}

TEST(SunSource, TheHdriOutranksTheClockWhichOutranksTheScene) {
    World w;
    const r::EnvironmentSettings procedural{};
    EXPECT_EQ(s::sun_source(w.reg, procedural, false), s::SunSource::clock);
    // A loaded HDRI the sky does not use leaves the clock in charge.
    EXPECT_EQ(s::sun_source(w.reg, procedural, true), s::SunSource::clock);
    // The HDRI sky without a loaded panorama falls back to the atmosphere.
    EXPECT_EQ(s::sun_source(w.reg, hdri_sky(), false), s::SunSource::clock);
    EXPECT_EQ(s::sun_source(w.reg, hdri_sky(), true), s::SunSource::hdri);
    // hdri.sun off: the HDRI lights as captured, the scene's sun stays.
    EXPECT_EQ(s::sun_source(w.reg, hdri_sky(false), true), s::SunSource::clock);

    w.wc().drive_sun = false;
    EXPECT_EQ(s::sun_source(w.reg, procedural, false), s::SunSource::manual);
    EXPECT_EQ(s::sun_source(w.reg, hdri_sky(), true), s::SunSource::hdri);
    w.wc().drive_sun = true;
    w.wc().enabled = false;
    EXPECT_EQ(s::sun_source(w.reg, procedural, false), s::SunSource::manual);

    World bare(/*with_clock=*/false);
    EXPECT_EQ(s::sun_source(bare.reg, procedural, false), s::SunSource::manual);
    EXPECT_EQ(s::sun_source(bare.reg, hdri_sky(), true), s::SunSource::hdri);

    EXPECT_EQ(s::sun_source_name(s::SunSource::hdri), "hdri");
    EXPECT_EQ(s::sun_source_name(s::SunSource::clock), "clock");
    EXPECT_EQ(s::sun_source_name(s::SunSource::manual), "manual");
}

TEST(SunSource, TheHdriSunLightsWhileTheClockKeepsTicking) {
    World w;
    const r::HdriSun pano = panorama_sun();
    const r::EnvironmentSettings env = hdri_sky();

    const s::SceneSun lit = s::resolve_sun(w.reg, env, &pano);
    EXPECT_EQ(lit.source, s::SunSource::hdri);
    ASSERT_TRUE(lit.light.has_value());
    // The panorama's sun, unrotated: the light travels away from it, with
    // the clamped-off irradiance as colour x intensity.
    expect_light_eq(*lit.light, {-pano.direction, pano.irradiance / 30.0F, 30.0F});

    // Three hours of clock time: the clock still advances its sky (stars,
    // moon) and keeps the scene's light current underneath, but the frame
    // stays lit by the HDRI's sun.
    const engine::astro::UtcTime before_utc = w.wc().utc;
    const glm::vec3 before_sky = w.reg.get<engine::astro::SkyState>(w.clock).sun_direction;
    const s::DirectionalLight before_light = w.dl();
    for (int i = 0; i < 3; ++i) s::world_clock_system(w.reg, 1.0F);
    EXPECT_NE(w.wc().utc, before_utc);
    EXPECT_GT(glm::length(w.reg.get<engine::astro::SkyState>(w.clock).sun_direction - before_sky),
              0.1F);
    EXPECT_GT(glm::length(w.dl().direction - before_light.direction), 0.1F);
    const s::SceneSun later = s::resolve_sun(w.reg, env, &pano);
    EXPECT_EQ(later.source, s::SunSource::hdri);
    ASSERT_TRUE(later.light.has_value());
    expect_light_eq(*later.light, *lit.light);

    // Back to the atmosphere: the clock's light, already current, takes over.
    const s::SceneSun sky = s::resolve_sun(w.reg, r::EnvironmentSettings{}, &pano);
    EXPECT_EQ(sky.source, s::SunSource::clock);
    ASSERT_TRUE(sky.light.has_value());
    expect_light_eq(*sky.light, w.dl());
}

TEST(SunSource, TheHdriSunFollowsTheEnvironmentsRotationAndIntensity) {
    World w(/*with_clock=*/false);
    const r::HdriSun pano = panorama_sun();
    r::EnvironmentSettings env = hdri_sky();
    env.hdri_rotation_deg = 90.0F;
    env.hdri_intensity = 2.0F;
    const s::SceneSun lit = s::resolve_sun(w.reg, env, &pano);
    ASSERT_TRUE(lit.light.has_value());
    const r::HdriSunLight expected = r::hdri_sun_light(pano, 90.0F, 2.0F);
    expect_light_eq(*lit.light, {-expected.direction_to_sun, expected.color, expected.intensity});
    // A quarter turn about +Y moves the sun off its panorama direction but
    // keeps its elevation.
    EXPECT_GT(glm::length(lit.light->direction + pano.direction), 0.5F);
    EXPECT_NEAR(lit.light->direction.y, -pano.direction.y, 1e-5F);
    EXPECT_NEAR(lit.light->intensity, 60.0F, 1e-3F);

    // Out-of-range settings are clamped as the bake clamps them.
    env.hdri_intensity = 1.0e4F;
    const s::SceneSun clamped = s::resolve_sun(w.reg, env, &pano);
    ASSERT_TRUE(clamped.light.has_value());
    EXPECT_NEAR(clamped.light->intensity, 30.0F * r::EnvironmentSettings::k_intensity.max, 1e-1F);
}

TEST(SunSource, ASunlessHdriPutsTheLightOutAndNoLightMeansNoSun) {
    World w;
    const r::HdriSun overcast{}; // found = false
    const s::SceneSun lit = s::resolve_sun(w.reg, hdri_sky(), &overcast);
    EXPECT_EQ(lit.source, s::SunSource::hdri);
    ASSERT_TRUE(lit.light.has_value());
    EXPECT_EQ(lit.light->intensity, 0.0F);

    // Without a DirectionalLight only the HDRI provides a sun.
    World bare(/*with_clock=*/false);
    bare.reg.destroy(bare.light);
    EXPECT_FALSE(s::resolve_sun(bare.reg, r::EnvironmentSettings{}, nullptr).light.has_value());
    const r::HdriSun pano = panorama_sun();
    EXPECT_TRUE(s::resolve_sun(bare.reg, hdri_sky(), &pano).light.has_value());
}

} // namespace
