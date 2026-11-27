// Global wind field CPU reference (R13.1).

#include <cmath>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/weather/weather_state.hpp"
#include "engine/weather/wind_field.hpp"

namespace {

namespace w = engine::weather;

[[nodiscard]] w::WeatherState breeze(float speed, float gustiness, float turbulence) {
    w::WeatherState s;
    s.wind_dir_deg = 30.0F;
    s.wind_speed = speed;
    s.gustiness = gustiness;
    s.turbulence = turbulence;
    return s;
}

TEST(WindField, CalmWeatherIsTheMeanWindEverywhere) {
    const w::WeatherState s = breeze(7.0F, 0.0F, 0.0F);
    const glm::vec2 dir(std::cos(glm::radians(30.0F)), std::sin(glm::radians(30.0F)));
    for (float x = -500.0F; x <= 500.0F; x += 97.0F) {
        const glm::vec3 v = w::wind_at(s, {x, 3.0F, -0.7F * x}, 123.0);
        EXPECT_NEAR(v.x, 7.0F * dir.x, 1e-5F);
        EXPECT_NEAR(v.y, 0.0F, 1e-6F);
        EXPECT_NEAR(v.z, 7.0F * dir.y, 1e-5F);
    }
    EXPECT_EQ(w::wind_at(breeze(0.0F, 1.0F, 1.0F), {10.0F, 0.0F, 3.0F}, 5.0), glm::vec3(0.0F));
}

TEST(WindField, GustsAndTurbulenceKeepTheMeanFlow) {
    const w::WeatherState s = breeze(10.0F, 0.6F, 0.5F);
    const glm::vec2 dir(std::cos(glm::radians(30.0F)), std::sin(glm::radians(30.0F)));
    glm::dvec3 sum(0.0);
    float lo = 1e9F;
    float hi = 0.0F;
    int n = 0;
    for (int i = 0; i < 120; ++i) {
        for (int j = 0; j < 120; ++j) {
            const glm::vec3 v = w::wind_at(s, {i * 7.3F, 0.0F, j * 6.1F}, 40.0);
            sum += glm::dvec3(v);
            const float h = glm::length(glm::vec2(v.x, v.z));
            lo = std::min(lo, h);
            hi = std::max(hi, h);
            ++n;
        }
    }
    const glm::dvec3 mean = sum / static_cast<double>(n);
    const glm::vec2 mean_h(static_cast<float>(mean.x), static_cast<float>(mean.z));
    // Blows along the heading at roughly the set speed...
    EXPECT_GT(glm::dot(glm::normalize(mean_h), dir), 0.99F);
    EXPECT_NEAR(glm::length(mean_h), 10.0F, 1.5F);
    EXPECT_NEAR(mean.y, 0.0, 0.2);
    // ...with gusts well above and lulls well below it.
    EXPECT_GT(hi, 13.0F);
    EXPECT_LT(lo, 8.0F);
}

TEST(WindField, GustFrontsTravelDownwindWithTheMeanWind) {
    // Without turbulence the field is a pure translation of the gust layer.
    const w::WeatherState s = breeze(12.0F, 0.8F, 0.0F);
    const glm::vec2 dir(std::cos(glm::radians(30.0F)), std::sin(glm::radians(30.0F)));
    const double t = 17.5;
    for (float x = 0.0F; x < 300.0F; x += 31.0F) {
        const glm::vec3 p(x, 0.0F, 0.4F * x - 50.0F);
        const glm::vec3 later =
            p + glm::vec3(dir.x, 0.0F, dir.y) * (12.0F * static_cast<float>(t));
        const glm::vec3 a = w::wind_at(s, p, 0.0);
        const glm::vec3 b = w::wind_at(s, later, t);
        EXPECT_NEAR(glm::length(a - b), 0.0F, 2e-3F) << x;
    }
}

TEST(WindField, AdvectionWrapIsSeamless) {
    // The lattice is periodic: advancing the clock by whole lattice periods
    // of both layers (lcm of the periods in metres) leaves the field unchanged.
    w::WeatherState s = breeze(9.0F, 0.7F, 0.0F);
    const double period_m =
        static_cast<double>(w::k_wind_lattice_period) * w::k_gust_along_m * w::k_turbulence_m;
    w::WindAdvection a;
    a.distance_m = 1234.5;
    w::WindAdvection b = a;
    b.distance_m += period_m;
    const w::WindFieldParams pa = w::wind_field_params(s, a);
    const w::WindFieldParams pb = w::wind_field_params(s, b);
    for (float x = -200.0F; x < 200.0F; x += 23.0F) {
        const w::WindSample sa = w::sample_wind(pa, {x, 2.0F * x});
        const w::WindSample sb = w::sample_wind(pb, {x, 2.0F * x});
        EXPECT_NEAR(glm::length(sa.velocity - sb.velocity), 0.0F, 1e-3F);
    }
}

TEST(WindField, IsContinuousInSpace) {
    const w::WeatherState s = breeze(15.0F, 1.0F, 1.0F);
    for (float x = -100.0F; x < 100.0F; x += 3.7F) {
        const glm::vec3 a = w::wind_at(s, {x, 0.0F, 11.0F}, 3.0);
        const glm::vec3 b = w::wind_at(s, {x + 0.01F, 0.0F, 11.0F}, 3.0);
        EXPECT_LT(glm::length(a - b), 0.05F) << x;
    }
}

TEST(WindField, LogProfileScalesWithHeight) {
    EXPECT_NEAR(w::wind_profile(w::k_wind_reference_height_m), 1.0F, 1e-6F);
    EXPECT_LT(w::wind_profile(0.5F), 0.6F);
    EXPECT_GT(w::wind_profile(0.5F), 0.4F);
    EXPECT_GT(w::wind_profile(100.0F), 1.3F);
    EXPECT_GT(w::wind_profile(0.0F), 0.0F); // clamped, never zero or negative
    EXPECT_LT(w::wind_profile(2.0F), w::wind_profile(4.0F));
}

} // namespace
