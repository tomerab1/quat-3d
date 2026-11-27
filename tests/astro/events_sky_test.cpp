// engine/astro/events.hpp (rise / set / transit / twilight) against USNO
// almanac times, and engine/astro/sky_state.hpp's world-space hand-off.

#include <cmath>
#include <cstdlib>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "astro/astro_test_util.hpp"
#include "engine/astro/coords.hpp"
#include "engine/astro/events.hpp"
#include "engine/astro/sky_state.hpp"
#include "engine/astro/sun.hpp"

namespace {

namespace a = engine::astro;
using quat::test::utc;

// |t − reference| in seconds; references are rounded to the minute.
[[nodiscard]] double seconds_off(const std::optional<a::UtcTime>& t, a::UtcTime reference) {
    if (!t) return 1e9;
    return std::abs(static_cast<double>(t->ns - reference.ns)) * 1e-9;
}

TEST(AstroEvents, SunriseAndSunsetMatchUsnoForBlufftonOhio) {
    // USNO one-day table, Bluffton OH (40.8939 N, 83.8917 W), 2018-09-12:
    // sunrise 11:13 UT, sunset 23:50 UT (as quoted in Skyfield's tests).
    // Local day = EDT (UTC−4).
    const a::UtcTime midnight = utc(2018, 9, 12, 4);
    const a::RiseSet r = a::rise_set(a::Body::sun, midnight, 40.8939, -83.8917);
    EXPECT_LE(seconds_off(r.rise, utc(2018, 9, 12, 11, 13)), 90.0);
    EXPECT_LE(seconds_off(r.set, utc(2018, 9, 12, 23, 50)), 90.0);
    // Solar noon = 12h UT − longitude (4 min per degree) − the equation of
    // time (Meeus 28; about +4 min that day).
    ASSERT_TRUE(r.transit.has_value());
    const double eot_min = a::sun_position(a::days_tt_since_j2000(*r.transit)).equation_of_time_min;
    EXPECT_NEAR(eot_min, 4.0, 0.5);
    const a::UtcTime noon = a::add_seconds(utc(2018, 9, 12, 12), (83.8917 * 4.0 - eot_min) * 60.0);
    EXPECT_LE(seconds_off(r.transit, noon), 2.0);
}

TEST(AstroEvents, TwilightMatchesUsnoForDefianceOhio) {
    // USNO, Defiance OH (41.281944 N, 84.362778 W), 2019-11-08, UT:
    // astronomical / nautical / civil dawn 10:42 / 11:15 / 11:48, sunrise
    // 12:17, sunset 22:25, civil / nautical / astronomical dusk 22:54 /
    // 23:27 / 23:59 (as quoted in Skyfield's tests). EST = UTC−5.
    const a::UtcTime midnight = utc(2019, 11, 8, 5);
    const double     lat = 41.281944;
    const double     lon = -84.362778;
    const auto       at = [&](double altitude) { return a::rise_set(a::Body::sun, midnight, lat, lon, altitude); };
    const a::RiseSet astro = at(a::k_astronomical_twilight_altitude);
    const a::RiseSet nautical = at(a::k_nautical_twilight_altitude);
    const a::RiseSet civil = at(a::k_civil_twilight_altitude);
    const a::RiseSet sun = at(a::k_sunrise_altitude);
    EXPECT_LE(seconds_off(astro.rise, utc(2019, 11, 8, 10, 42)), 90.0);
    EXPECT_LE(seconds_off(nautical.rise, utc(2019, 11, 8, 11, 15)), 90.0);
    EXPECT_LE(seconds_off(civil.rise, utc(2019, 11, 8, 11, 48)), 90.0);
    EXPECT_LE(seconds_off(sun.rise, utc(2019, 11, 8, 12, 17)), 90.0);
    EXPECT_LE(seconds_off(sun.set, utc(2019, 11, 8, 22, 25)), 90.0);
    EXPECT_LE(seconds_off(civil.set, utc(2019, 11, 8, 22, 54)), 90.0);
    EXPECT_LE(seconds_off(nautical.set, utc(2019, 11, 8, 23, 27)), 90.0);
    EXPECT_LE(seconds_off(astro.set, utc(2019, 11, 8, 23, 59)), 90.0);
}

TEST(AstroEvents, GrazingMoonriseAtSeventyNorth) {
    // 70°N 0°E, 2023-02-19: the Moon is up only from 11:28 to 12:05 UT
    // (Skyfield / JPL DE421). A grazing event: the altitude changes slowly,
    // so a small position error moves the times by minutes.
    const a::RiseSet r = a::rise_set(a::Body::moon, utc(2023, 2, 19), 70.0, 0.0);
    EXPECT_LE(seconds_off(r.rise, utc(2023, 2, 19, 11, 28)), 180.0);
    EXPECT_LE(seconds_off(r.set, utc(2023, 2, 19, 12, 5)), 180.0);
}

TEST(AstroEvents, PolarDayAndNightHaveNoCrossings) {
    // Midsummer at 80°N: the Sun never sets; midwinter: it never rises.
    const a::RiseSet summer = a::rise_set(a::Body::sun, utc(2026, 6, 21), 80.0, 0.0);
    EXPECT_TRUE(summer.always_above);
    EXPECT_FALSE(summer.rise.has_value());
    EXPECT_FALSE(summer.set.has_value());
    const a::RiseSet winter = a::rise_set(a::Body::sun, utc(2026, 12, 21), 80.0, 0.0);
    EXPECT_TRUE(winter.always_below);
    EXPECT_TRUE(winter.transit.has_value()); // it still crosses the meridian
}

TEST(AstroEvents, LocalMidnightRespectsTheOffset) {
    EXPECT_EQ(a::local_midnight(utc(2026, 6, 21, 1, 30), 120), utc(2026, 6, 20, 22));
    EXPECT_EQ(a::local_midnight(utc(2026, 6, 21, 23, 30), -300), utc(2026, 6, 21, 5));
    EXPECT_EQ(a::local_midnight(utc(1999, 12, 31, 23), 0), utc(1999, 12, 31));
}

TEST(AstroSky, SunDirectionFollowsTheDayInWorldSpace) {
    // Paris (48.8566 N, 2.3522 E) at the 2026 June solstice: the Sun rises
    // in the north-east, culminates due south at 90 − 48.86 + 23.44 = 64.6°
    // and sets in the north-west. World: east = +X, north = −Z, up = +Y.
    const a::Observer paris{48.8566, 2.3522, 0.0};
    const a::RiseSet  day = a::rise_set(a::Body::sun, utc(2026, 6, 20, 22), paris.latitude, paris.longitude);
    ASSERT_TRUE(day.rise && day.set && day.transit);

    const a::SkyState noon = a::compute_sky_state(*day.transit, paris);
    EXPECT_NEAR(noon.sun_azimuth_deg, 180.0, 0.05);
    EXPECT_NEAR(noon.sun_elevation_deg, 90.0 - 48.8566 + 23.44, 0.05);
    EXPECT_GT(noon.sun_direction.z, 0.0F); // south = +Z
    EXPECT_NEAR(noon.sun_direction.x, 0.0F, 1e-3F);
    EXPECT_FLOAT_EQ(noon.sun_disc_visible, 1.0F);

    const a::SkyState rise = a::compute_sky_state(*day.rise, paris);
    EXPECT_NEAR(rise.sun_azimuth_deg, 51.0, 2.0);
    // Upper limb on the horizon (the −50' convention assumes 34' of
    // refraction; Sæmundsson gives 37' there, lifting the disc ~3').
    EXPECT_NEAR(rise.sun_elevation_deg, -rise.sun_angular_radius_deg, 0.06);
    EXPECT_GT(rise.sun_direction.x, 0.0F); // east
    EXPECT_LT(rise.sun_direction.z, 0.0F); // north
    EXPECT_LT(rise.sun_disc_visible, 0.1F);

    const a::SkyState set = a::compute_sky_state(*day.set, paris);
    EXPECT_LT(set.sun_direction.x, 0.0F); // west
    EXPECT_LT(set.sun_direction.z, 0.0F); // north

    const a::SkyState night = a::compute_sky_state(utc(2026, 6, 21, 0, 0), paris);
    EXPECT_LT(night.sun_elevation_deg, -10.0F);
    EXPECT_FLOAT_EQ(night.sun_disc_visible, 0.0F);
    EXPECT_NEAR(glm::length(night.sun_direction), 1.0F, 1e-5F);
}

TEST(AstroSky, NorthOffsetRotatesEveryDirection) {
    const a::UtcTime t = utc(2026, 3, 20, 12);
    const a::SkyState s0 = a::compute_sky_state(t, {51.5, 0.0, 0.0});
    const a::SkyState s90 = a::compute_sky_state(t, {51.5, 0.0, 90.0});
    // Turning compass north from −Z to +X maps (x, z) -> (−z, x).
    EXPECT_NEAR(s90.sun_direction.x, -s0.sun_direction.z, 1e-5F);
    EXPECT_NEAR(s90.sun_direction.z, s0.sun_direction.x, 1e-5F);
    EXPECT_NEAR(s90.moon_direction.y, s0.moon_direction.y, 1e-5F);
    EXPECT_FLOAT_EQ(s90.sun_azimuth_deg, s0.sun_azimuth_deg);
}

TEST(AstroSky, CelestialPoleStandsAtTheLatitudeDueNorth) {
    // The J2000 pole is within the precession since 2000 (≈ 0.36° by 2026)
    // of the true pole, whose elevation equals the latitude.
    for (double lat : {-33.9, 0.0, 48.8566, 70.0}) {
        const a::SkyState s = a::compute_sky_state(utc(2026, 11, 27, 21), {lat, 2.35, 0.0});
        const glm::vec3   pole = s.world_from_celestial[2];
        EXPECT_NEAR(std::asin(pole.y) / a::k_deg, lat, 0.5) << lat;
        if (lat > 1.0) EXPECT_LT(pole.z, 0.0F) << lat; // toward north (−Z)
        // A rotation: orthonormal, right-handed.
        EXPECT_NEAR(glm::determinant(s.world_from_celestial), 1.0F, 1e-5F);
    }
}

TEST(AstroSky, MoonHandOffIsConsistent) {
    const a::SkyState s = a::compute_sky_state(utc(2018, 9, 16, 23, 15), {40.0, -80.0, 0.0});
    // First quarter: half lit, waxing, sunlight arriving at ~90° to the
    // line of sight (within the topocentric parallax, ≤ 1°).
    EXPECT_NEAR(s.moon_illuminated_fraction, 0.5F, 0.02F);
    EXPECT_TRUE(s.moon_waxing);
    const float cos_phase = glm::dot(s.moon_sun_direction, -s.moon_direction);
    EXPECT_NEAR(std::acos(cos_phase) / static_cast<float>(a::k_deg), s.moon_phase_angle_deg, 1.5F);
    EXPECT_NEAR(glm::determinant(s.moon_orientation), 1.0F, 1e-5F);
    // The mean sub-Earth point (body +X) faces the observer to within the
    // libration (< 10°) plus the topocentric parallax (≤ 1°).
    EXPECT_GT(glm::dot(s.moon_orientation[0], -s.moon_direction), std::cos(12.0F * 0.0174533F));
    EXPECT_GT(s.moon_illuminance_lux, 0.02F);
    EXPECT_LT(s.moon_illuminance_lux, 0.05F);
}

TEST(AstroSky, DiscFractionIsACircularSegment) {
    EXPECT_DOUBLE_EQ(a::disc_fraction_above_horizon(-0.3, 0.25), 0.0);
    EXPECT_NEAR(a::disc_fraction_above_horizon(0.0, 0.25), 0.5, 1e-12);
    EXPECT_DOUBLE_EQ(a::disc_fraction_above_horizon(0.3, 0.25), 1.0);
    EXPECT_NEAR(a::disc_fraction_above_horizon(0.1, 0.25) + a::disc_fraction_above_horizon(-0.1, 0.25), 1.0,
                1e-12);
}

} // namespace
