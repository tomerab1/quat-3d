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
    const float mid_m = (clouds.bottom_km + 0.5F * clouds.thickness_km) * 1000.0F;
    EXPECT_NEAR(v.x, 5.0F * w::wind_profile(mid_m), 1e-4F);
    EXPECT_GT(v.x, 5.0F); // faster aloft
}

TEST(WeatherSurfaces, PuddlesStartOnceSoaked) {
    r::WeatherSurfaces ws;
    ws.wetness = 0.5F;
    EXPECT_EQ(ws.puddle_level(), 0.0F);
    ws.wetness = 1.0F;
    EXPECT_NEAR(ws.puddle_level(), 0.75F, 1e-6F);
    ws.wetness = 0.8F;
    EXPECT_GT(ws.puddle_level(), 0.0F);
    EXPECT_LT(ws.puddle_level(), 0.75F);
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

} // namespace
