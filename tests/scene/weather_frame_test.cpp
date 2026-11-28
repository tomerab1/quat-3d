// The weather a frame renders with (R13.1): scene weather vs defaults, the
// clouds' wind, and what the lighting pass and cloud layer receive.

#include <cmath>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/renderer/cloud_settings.hpp"
#include "engine/renderer/weather_surfaces.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/weather_frame.hpp"
#include "engine/weather/weather_state.hpp"
#include "engine/weather/wind_field.hpp"

namespace {

namespace s = engine::scene;
namespace w = engine::weather;
namespace r = engine::renderer;

TEST(WeatherFrame, SceneWeatherWins) {
    entt::registry reg;
    reg.emplace<s::Grass>(reg.create()).settings.wind_direction = {0.0F, 1.0F};
    w::WeatherState& state = reg.emplace<w::WeatherState>(reg.create());
    state.wind_dir_deg = -45.0F;
    state.wind_speed = 99.0F; // sanitized
    w::WeatherState out;
    EXPECT_TRUE(s::frame_weather(reg, out));
    EXPECT_FLOAT_EQ(out.wind_dir_deg, -45.0F);
    EXPECT_FLOAT_EQ(out.wind_speed, 60.0F);
}

TEST(WeatherFrame, WithoutWeatherTheGrassKeepsItsWindHeading) {
    entt::registry reg;
    w::WeatherState out;
    EXPECT_FALSE(s::frame_weather(reg, out));
    EXPECT_FLOAT_EQ(out.wind_dir_deg, w::WeatherState{}.wind_dir_deg);
    reg.emplace<s::Grass>(reg.create()).settings.wind_direction = {0.0F, 1.0F};
    EXPECT_FALSE(s::frame_weather(reg, out));
    EXPECT_NEAR(out.wind_dir_deg, 90.0F, 1e-4F);
    EXPECT_FLOAT_EQ(out.wetness, 0.0F); // dry
}

TEST(WeatherFrame, CloudsDriftWithTheMeanWindAloft) {
    w::WeatherState state;
    state.wind_dir_deg = 0.0F;
    state.wind_speed = 5.0F;
    state.gustiness = 1.0F; // gusts do not move the clouds
    const r::CloudSettings clouds;
    const glm::vec2 v = s::cloud_wind(state, clouds);
    EXPECT_NEAR(v.y, 0.0F, 1e-5F);
    EXPECT_NEAR(v.x, 5.0F * w::k_geostrophic_factor, 1e-4F); // free atmosphere
    EXPECT_GT(v.x, 5.0F); // faster aloft
}

TEST(WeatherSurfaces, PuddlesStartOnceSoaked) {
    r::WeatherSurfaces ws;
    ws.wetness = 0.5F;
    EXPECT_EQ(ws.puddle_level(), 0.0F);
    ws.wetness = 1.0F;
    EXPECT_NEAR(ws.puddle_level(), 0.45F, 1e-6F);
    ws.wetness = 0.8F;
    EXPECT_GT(ws.puddle_level(), 0.0F);
    EXPECT_LT(ws.puddle_level(), 0.45F);
    ws.wetness = std::nanf("");
    ws.snow = 3.0F;
    const r::WeatherSurfaces clean = ws.sanitized();
    EXPECT_EQ(clean.wetness, 0.0F);
    EXPECT_EQ(clean.snow, 1.0F);
}

TEST(CloudSettings, OffsetAnchorsTheFieldToTheWorld) {
    r::CloudSettings c;
    c.time_s = 0.0F;
    const glm::vec4 base = c.pack_b();
    c.offset_km = {1.5F, -2.0F};
    const glm::vec4 moved = c.pack_b();
    EXPECT_FLOAT_EQ(moved.z - base.z, 1.5F);
    EXPECT_FLOAT_EQ(moved.w - base.w, -2.0F);
    EXPECT_FLOAT_EQ(moved.x, base.x);
}

// Scenes without weather render as before R13.1: the clouds the frame packs
// are exactly master's (settings + time-driven drift, camera-relative), and
// the cloud shadow map is not built (frame_graph gates it on weather).
TEST(WeatherFrame, WithoutWeatherTheCloudsAreUnchanged) {
    r::CloudSettings settings;
    settings.coverage = 0.63F;
    settings.wind_speed = 17.0F;
    settings.wind_dir_deg = -48.0F;
    const float time_s = 1234.5F;
    const r::CloudSettings c = s::frame_clouds(settings, time_s, nullptr, {7.0F, -3.0F},
                                               glm::vec3(4000.0F, 12.0F, -2500.0F));
    EXPECT_EQ(c.offset_km, glm::vec2(0.0F)) << "camera-relative, no weather drift";
    EXPECT_EQ(c.time_s, time_s);
    // pack_a / pack_b exactly as master computed them.
    const float heading = glm::radians(settings.wind_dir_deg);
    const float drift_km = settings.wind_speed * time_s * 0.001F;
    EXPECT_EQ(c.pack_a(), glm::vec4(settings.coverage, settings.density, settings.bottom_km,
                                    settings.thickness_km));
    EXPECT_EQ(c.pack_b(), glm::vec4(1.0F / settings.size_km, settings.detail,
                                    drift_km * std::cos(heading), drift_km * std::sin(heading)));
}

TEST(WeatherFrame, WithWeatherTheCloudsFollowItAndStayAnchored) {
    r::CloudSettings settings;
    w::WeatherState state;
    state.cloud_cover = 0.9F;
    state.precipitation = 0.5F;
    const glm::vec3 camera(4000.0F, 12.0F, -2500.0F);
    const r::CloudSettings c = s::frame_clouds(settings, 99.0F, &state, {7.0F, -3.0F}, camera);
    EXPECT_FLOAT_EQ(c.coverage, 0.9F);
    EXPECT_FLOAT_EQ(c.density, settings.density * 1.5F);
    EXPECT_EQ(c.time_s, 0.0F) << "drift comes from the integrated weather wind";
    EXPECT_FLOAT_EQ(c.offset_km.x, 7.0F + 4.0F);
    EXPECT_FLOAT_EQ(c.offset_km.y, -3.0F - 2.5F);
}

} // namespace
