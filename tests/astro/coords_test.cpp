// engine/astro/coords.hpp against the worked examples of Meeus,
// "Astronomical Algorithms", 2nd ed. (1998).

#include <cmath>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "astro/astro_test_util.hpp"
#include "engine/astro/coords.hpp"
#include "engine/astro/time.hpp"

namespace {

namespace a = engine::astro;
using quat::test::dms;
using quat::test::hms;
using quat::test::utc;

TEST(AstroCoords, SiderealTimeMatchesExamples12aAnd12b) {
    // 12.a: 1987 April 10, 0h UT -> 13h10m46.3668s.
    EXPECT_NEAR(a::greenwich_mean_sidereal_time(a::days_ut_since_j2000(utc(1987, 4, 10))),
                hms(13, 10, 46.3668), 1e-6);
    // 12.b: 1987 April 10, 19h21m00s UT -> 8h34m57.0896s.
    EXPECT_NEAR(a::greenwich_mean_sidereal_time(a::days_ut_since_j2000(utc(1987, 4, 10, 19, 21))),
                hms(8, 34, 57.0896), 1e-6);
}

TEST(AstroCoords, NutationAndObliquityMatchExample22a) {
    // 22.a: 1987 April 10, 0h TD: Δψ = −3.788", Δε = +9.443",
    // ε0 = 23°26'27.407" (the low-precision series is good to 0.5" / 0.1").
    const double T = a::centuries_tt(quat::test::days_from_jde(2446895.5));
    const a::Nutation n = a::nutation(T);
    EXPECT_NEAR(n.longitude * 3600.0, -3.788, 0.5);
    EXPECT_NEAR(n.obliquity * 3600.0, 9.443, 0.1);
    EXPECT_NEAR(a::mean_obliquity(T), dms(23, 26, 27.407), 0.001 / 3600.0);
}

TEST(AstroCoords, HorizontalCoordinatesMatchExample13b) {
    // 13.b: Venus at the USNO, 1987 April 10 19h21m UT. Apparent sidereal
    // time 8h34m56.853s, longitude 77°03'56" W, latitude 38°55'17" N.
    // Meeus' A = 68.0337° is measured from the south; compass = A + 180°.
    const double lst = hms(8, 34, 56.853) - dms(77, 3, 56);
    const a::Equatorial venus{hms(23, 9, 16.641), dms(6, 43, 11.61, true)};
    const a::Horizontal h = a::horizontal_from_equatorial(venus, lst, dms(38, 55, 17));
    EXPECT_NEAR(h.azimuth, 68.0337 + 180.0, 2e-4); // Meeus prints 4 decimals
    EXPECT_NEAR(h.elevation, 15.1249, 1e-4);
}

TEST(AstroCoords, EclipticToEquatorialMatchesExample13a) {
    // Exercise of 13.a: λ = 113.21563°, β = 6.68417°, ε = 23.4392911° ->
    // α = 7h45m18.946s, δ = +28°01'34.26".
    const a::Equatorial q = a::equatorial_from_ecliptic(113.21563, 6.68417, 23.4392911);
    EXPECT_NEAR(q.right_ascension, hms(7, 45, 18.946), 2e-5);
    EXPECT_NEAR(q.declination, dms(28, 1, 34.26), 2e-5);
}

TEST(AstroCoords, PrecessionMatchesExample21bAndThePolarisExercise) {
    // 21.b: θ Persei (J2000 position plus 28.86705 years of proper motion:
    // α0 = 41.054063°, δ0 = 49.227750°) to 2028 Nov 13.19 TD ->
    // α = 2h46m11.331s, δ = +49°20'54.54".
    const double T = a::centuries_tt(quat::test::days_from_jde(2462088.69));
    a::Equatorial q = a::equatorial_from_vector(a::precession_from_j2000(T) *
                                                a::equatorial_vector({41.054063, 49.227750}));
    EXPECT_NEAR(q.right_ascension, hms(2, 46, 11.331), 3e-5);
    EXPECT_NEAR(q.declination, dms(49, 20, 54.54), 3e-5);

    // Polaris (exercise p. 136): J2000 α = 2h31m48.704s, δ = +89°15'50.72",
    // proper motion +0.19877 s / −0.0152" per year -> 2100.0:
    // α = 5h53m29.17s, δ = +89°32'22.18". Near the pole an error in the
    // matrix shows up magnified in α.
    const a::Equatorial polaris_2100{hms(2, 31, 48.704 + 0.19877 * 100.0),
                                     dms(89, 15, 50.72 - 0.0152 * 100.0)};
    q = a::equatorial_from_vector(a::precession_from_j2000(1.0) * a::equatorial_vector(polaris_2100));
    EXPECT_NEAR(q.right_ascension, hms(5, 53, 29.17), 0.02 * 15.0 / 3600.0);
    EXPECT_NEAR(q.declination, dms(89, 32, 22.18), 0.02 / 3600.0);
}

TEST(AstroCoords, RefractionMatchesExample16a) {
    // 16.a: true elevation 33.246' -> R = 24.618' (Sæmundsson).
    EXPECT_NEAR(a::refraction(33.246 / 60.0) * 60.0, 24.618, 0.01);
    EXPECT_NEAR(a::refraction(90.0) * 3600.0, 0.0, 0.1);
    // ~34' at the horizon (the standard sunrise allowance), continuous and
    // monotonic below it.
    EXPECT_NEAR(a::refraction(-0.57) * 60.0, 34.0, 1.5);
    double prev = a::apparent_elevation(-6.0);
    for (double h = -5.9; h < 5.0; h += 0.1) {
        const double app = a::apparent_elevation(h);
        EXPECT_GT(app, prev) << h;
        prev = app;
    }
    EXPECT_DOUBLE_EQ(a::refraction(-5.0), 0.0);
}

TEST(AstroCoords, AirmassFollowsKastenYoung) {
    EXPECT_NEAR(a::airmass(90.0), 1.0, 1e-3);
    EXPECT_NEAR(a::airmass(30.0), 1.995, 0.01);  // ≈ sec z away from the horizon
    EXPECT_NEAR(a::airmass(0.0), 37.92, 0.1);    // Kasten & Young 1989 table
    EXPECT_DOUBLE_EQ(a::airmass(-10.0), a::airmass(0.0));
    EXPECT_NEAR(a::extinction_factor(1.0, 0.172), 0.8535, 1e-3);
}

TEST(AstroCoords, WorldAxesAreRightHandedWithNorthAlongMinusZ) {
    const glm::dmat3 m = a::world_from_enu(0.0);
    EXPECT_EQ(m * glm::dvec3(1, 0, 0), glm::dvec3(1, 0, 0));  // east = +X
    EXPECT_EQ(m * glm::dvec3(0, 1, 0), glm::dvec3(0, 0, -1)); // north = −Z
    EXPECT_EQ(m * glm::dvec3(0, 0, 1), glm::dvec3(0, 1, 0));  // up = +Y
    EXPECT_NEAR(glm::determinant(m), 1.0, 1e-12);
    // A 90° offset turns north clockwise from above, onto +X.
    const glm::dvec3 north = a::world_from_enu(90.0) * glm::dvec3(0, 1, 0);
    EXPECT_NEAR(glm::distance(north, glm::dvec3(1, 0, 0)), 0.0, 1e-12);
    const glm::dvec3 east = a::world_from_enu(90.0) * glm::dvec3(1, 0, 0);
    EXPECT_NEAR(glm::distance(east, glm::dvec3(0, 0, 1)), 0.0, 1e-12);
}

TEST(AstroCoords, ObserverParallaxMatchesMeeusChapter11) {
    // Ch. 11 example, Palomar (φ = 33°21'22", H = 1706 m): ρ sin φ' =
    // 0.546861, ρ cos φ' = 0.836339. At sea level ρ is H / a smaller.
    const glm::dvec3 o = a::observer_offset_enu(dms(33, 21, 22)) / a::k_earth_equatorial_radius_km;
    EXPECT_NEAR(glm::length(o), std::hypot(0.546861, 0.836339) - 1706.0 / 6378140.0, 2e-6);
    // Geocentric latitude is lower than geodetic: seen from the local
    // vertical, the radius vector leans toward the equator (south here).
    EXPECT_LT(o.y, 0.0);
    EXPECT_NEAR(std::atan2(-o.y, o.z) / a::k_deg, dms(33, 21, 22) - std::atan2(0.546861, 0.836339) / a::k_deg,
                0.002);
    EXPECT_DOUBLE_EQ(o.x, 0.0);
}

} // namespace
