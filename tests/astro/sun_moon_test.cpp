// engine/astro/sun.hpp and moon.hpp against Meeus' worked examples and
// USNO almanac data.

#include <array>
#include <cmath>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "astro/astro_test_util.hpp"
#include "engine/astro/coords.hpp"
#include "engine/astro/moon.hpp"
#include "engine/astro/sun.hpp"

namespace {

namespace a = engine::astro;
using quat::test::days_from_jde;
using quat::test::dms;
using quat::test::hms;
using quat::test::utc;

TEST(AstroSun, MatchesExample25a) {
    // Meeus 25.a: 1992 October 13.0 TD (JDE 2448908.5), low-accuracy
    // method: λ = 199°54'32" (199.90895), R = 0.99766 AU,
    // α = 13h13m31.4s, δ = −7°47'06".
    const a::SunPosition s = a::sun_position(days_from_jde(2448908.5));
    EXPECT_NEAR(s.longitude, 199.90895, 2e-5);
    EXPECT_NEAR(s.distance_au, 0.99766, 1e-5);
    EXPECT_NEAR(s.equatorial.right_ascension, hms(13, 13, 31.4), 0.05 * 15.0 / 3600.0);
    EXPECT_NEAR(s.equatorial.declination, dms(7, 47, 6, true), 0.5 / 3600.0);
    // Against the full VSOP87 result of Example 25.b (α = 13h13m30.749s,
    // δ = −7°47'01.74") the method is good to ~0.01° as advertised.
    EXPECT_NEAR(s.equatorial.right_ascension, hms(13, 13, 30.749), 0.01);
    EXPECT_NEAR(s.equatorial.declination, dms(7, 47, 1.74, true), 0.01);
}

TEST(AstroSun, EquationOfTimeMatchesExample28b) {
    // 28.b: same instant, E = +13m42.7s (Smart's formula).
    EXPECT_NEAR(a::sun_position(days_from_jde(2448908.5)).equation_of_time_min, 13.0 + 42.7 / 60.0,
                0.1 / 60.0);
}

TEST(AstroSun, EquinoxAndSolsticeFallOnTheUsnoInstants) {
    // USNO "Earth's seasons" for 2018: September equinox 09-23 01:54 UT,
    // December solstice 12-21 22:23 UT. The Sun's apparent longitude is
    // 180° / 270° then (it moves 0.04°/h; 0.01° is 15 minutes).
    auto lon = [](a::UtcTime t) { return a::sun_position(a::days_tt_since_j2000(t)).longitude; };
    EXPECT_NEAR(lon(utc(2018, 9, 23, 1, 54)), 180.0, 0.01);
    EXPECT_NEAR(lon(utc(2018, 12, 21, 22, 23)), 270.0, 0.01);
    // At the equinox the declination is zero; at the solstice it is −ε.
    const a::SunPosition sol = a::sun_position(a::days_tt_since_j2000(utc(2018, 12, 21, 22, 23)));
    EXPECT_NEAR(sol.equatorial.declination, -sol.true_obliquity, 1e-4);
}

TEST(AstroSun, IlluminanceIsTheLuminousSolarConstantOverRSquared) {
    EXPECT_NEAR(a::sun_illuminance_lux(1.0), 133334.0, 1e-6);
    EXPECT_NEAR(a::sun_illuminance_lux(0.983), 133334.0 / (0.983 * 0.983), 1e-6); // perihelion
    EXPECT_NEAR(a::sun_angular_radius(1.0) * 60.0, 15.99, 0.01);
}

TEST(AstroMoon, MatchesExample47a) {
    // 47.a: 1992 April 12, 0h TD (JDE 2448724.5).
    const double         d = days_from_jde(2448724.5);
    const a::MoonSeriesSums sums = a::moon_series_sums(d);
    EXPECT_NEAR(sums.l, -1127527.0, 1.0);
    EXPECT_NEAR(sums.b, -3229126.0, 1.0);
    EXPECT_NEAR(sums.r, -16590875.0, 1.0);
    const a::MoonPosition m = a::moon_position(d);
    // λ = 133.162655° geometric (+ Δψ = +0.004610° -> 133.167265° apparent),
    // β = −3.229126°, Δ = 368409.7 km, π = 0.991990°,
    // α = 134.688470°, δ = +13.768368°.
    EXPECT_NEAR(m.longitude - m.nutation_longitude, 133.162655, 2e-6);
    EXPECT_NEAR(m.longitude, 133.167265, 1e-4); // low-precision nutation: 0.5"
    EXPECT_NEAR(m.latitude, -3.229126, 2e-6);
    EXPECT_NEAR(m.distance_km, 368409.7, 0.1);
    EXPECT_NEAR(m.parallax, 0.991990, 2e-6);
    EXPECT_NEAR(m.equatorial.right_ascension, 134.688470, 2e-4);
    EXPECT_NEAR(m.equatorial.declination, 13.768368, 2e-4);
}

TEST(AstroMoon, PhaseMatchesExample48a) {
    // 48.a: the same instant: i = 69.0756°, k = 0.6786, χ = 285.0°.
    const double d = days_from_jde(2448724.5);
    const a::MoonPhase p = a::moon_phase(a::moon_position(d), a::sun_position(d));
    EXPECT_NEAR(p.phase_angle, 69.0756, 0.01);
    EXPECT_NEAR(p.illuminated_fraction, 0.6786, 2e-4);
    EXPECT_NEAR(p.bright_limb, 285.0, 0.1);
    EXPECT_TRUE(p.waxing);
}

TEST(AstroMoon, IlluminatedFractionMatchesUsnoTable) {
    // USNO "Fraction of the Moon illuminated", September 2018 at 05:00 UT
    // (EST midnight), days 9..18, as quoted in Skyfield's test suite.
    constexpr std::array k_usno = {0.00, 0.00, 0.03, 0.08, 0.15, 0.24, 0.33, 0.43, 0.52, 0.62};
    for (int i = 0; i < 10; ++i) {
        const double d = a::days_tt_since_j2000(utc(2018, 9, 9 + i, 5));
        const a::MoonPhase p = a::moon_phase(a::moon_position(d), a::sun_position(d));
        EXPECT_NEAR(p.illuminated_fraction, k_usno[static_cast<std::size_t>(i)], 0.0051) << "day " << 9 + i;
    }
}

TEST(AstroMoon, QuarterAndFullMoonFallOnTheUsnoInstants) {
    // USNO phases: first quarter 2018-09-16 23:15 UT, full 2018-09-25 02:52 UT.
    // The Moon gains 0.5°/h on the Sun; 0.05° is 6 minutes.
    auto elongation = [](a::UtcTime t) {
        const double d = a::days_tt_since_j2000(t);
        return a::wrap_360(a::moon_position(d).longitude - a::sun_position(d).longitude);
    };
    EXPECT_NEAR(elongation(utc(2018, 9, 16, 23, 15)), 90.0, 0.05);
    EXPECT_NEAR(elongation(utc(2018, 9, 25, 2, 52)), 180.0, 0.05);
}

TEST(AstroMoon, LibrationMatchesExample53a) {
    // 53.a: 1992 April 12, 0h TD: optical libration l' = −1.206°,
    // b' = +4.194°; P = 15.08° (with physical libration; ρ, σ are ~0.03°).
    const a::MoonLibration lib = a::moon_libration(a::moon_position(days_from_jde(2448724.5)));
    EXPECT_NEAR(lib.longitude, -1.206, 0.002);
    EXPECT_NEAR(lib.latitude, 4.194, 0.002);
    EXPECT_NEAR(lib.position_angle, 15.08, 0.05);
}

TEST(AstroMoon, BodyAxesPutTheSubEarthPointAndPoleWhereLibrationSays) {
    const a::MoonPosition m = a::moon_position(days_from_jde(2448724.5));
    const a::MoonLibration lib = a::moon_libration(m);
    const glm::dvec3 d = a::equatorial_vector(m.equatorial);
    const glm::dmat3 axes = a::moon_body_axes(d, lib);
    EXPECT_NEAR(glm::determinant(axes), 1.0, 1e-12);
    for (int i = 0; i < 3; ++i) EXPECT_NEAR(glm::length(axes[i]), 1.0, 1e-12);
    // Sub-Earth point in body coordinates.
    const glm::dvec3 to_earth = glm::transpose(axes) * -d;
    EXPECT_NEAR(std::atan2(to_earth.y, to_earth.x) / a::k_deg, lib.longitude, 1e-9);
    EXPECT_NEAR(std::asin(to_earth.z) / a::k_deg, lib.latitude, 1e-9);
    // Position angle of the projected pole on the sky.
    const glm::dvec3 pole = axes[2];
    const glm::dvec3 north = glm::normalize(glm::dvec3(0, 0, 1) - d.z * d);
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), d));
    const double     pa = std::atan2(glm::dot(pole, east), glm::dot(pole, north)) / a::k_deg;
    EXPECT_NEAR(pa, lib.position_angle, 1e-9);
}

TEST(AstroMoon, IlluminanceFollowsKrisciunasSchaefer) {
    // Full Moon at mean distance: 10^(−0.4·3.84) fc = 0.313 lx outside the
    // atmosphere; at the zenith through k = 0.172 mag/airmass (Krisciunas &
    // Schaefer's V-band value) 0.29 lx — inside the owner's 0.1–0.3 lx range
    // for full-moon ground illuminance.
    const double full = a::moon_illuminance_lux(0.0, a::k_moon_mean_distance_km);
    EXPECT_NEAR(full, 0.313, 0.002);
    const double ground = full * a::extinction_factor(a::airmass(90.0), 0.172);
    EXPECT_GE(ground, 0.1);
    EXPECT_LE(ground, 0.3);
    // A quarter Moon is about a tenth of full (the opposition surge makes
    // the full Moon far brighter than a Lambert sphere would be).
    const double quarter = a::moon_illuminance_lux(90.0, a::k_moon_mean_distance_km);
    EXPECT_NEAR(quarter / full, 0.08, 0.02);
    EXPECT_LT(a::moon_illuminance_lux(170.0, a::k_moon_mean_distance_km), 1e-3);
    // Perigee is brighter (inverse square).
    EXPECT_NEAR(a::moon_illuminance_lux(0.0, 356500.0) / full, std::pow(384400.0 / 356500.0, 2.0), 1e-12);
}

} // namespace
