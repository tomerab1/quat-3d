#pragma once

// Coordinate systems and Earth rotation (ROADMAP R22.1): obliquity,
// nutation, precession, sidereal time, equatorial <-> horizontal, the
// observer's parallax, atmospheric refraction, airmass and the mapping of
// the local horizon frame onto the engine's world axes.
//
// Conventions:
//   - Angles in degrees unless a name says otherwise; doubles throughout.
//   - Azimuth is the compass bearing: 0 = north, 90 = east (Meeus measures
//     from the south; converted here).
//   - The local horizon frame is ENU: x = east, y = north, z = up.
//   - World axes (engine convention, y up, right-handed): with
//     north_offset_deg = 0, north = -Z, east = +X, up = +Y. A non-zero
//     offset turns compass north clockwise (seen from above) by that angle:
//     north = (sin θ, 0, -cos θ), east = (cos θ, 0, sin θ).
//   - Equatorial vectors: x toward the vernal equinox, z toward the
//     celestial north pole.
//
// References: Meeus, "Astronomical Algorithms", 2nd ed. (1998): ch. 12
// (sidereal time), 13 (transformation of coordinates), 16 (refraction,
// Sæmundsson's formula), 21 (precession, IAU 1976), 22 (nutation, low
// precision); Kasten & Young, "Revised optical air mass tables and
// approximation formula", Applied Optics 28(22), 1989.

#include <glm/glm.hpp>

namespace engine::astro {

inline constexpr double k_pi = 3.14159265358979323846;
inline constexpr double k_deg = k_pi / 180.0;
inline constexpr double k_km_per_au = 149597870.7;
inline constexpr double k_earth_equatorial_radius_km = 6378.14;

// `deg` reduced to [0, 360).
[[nodiscard]] double wrap_360(double deg);
// `deg` reduced to (-180, 180].
[[nodiscard]] double wrap_180(double deg);

// Mean obliquity of the ecliptic (Meeus 22.2), T in Julian centuries TT.
[[nodiscard]] double mean_obliquity(double T);

// Nutation in longitude Δψ and obliquity Δε (Meeus ch. 22, low precision:
// 0.5" / 0.1").
struct Nutation {
    double longitude = 0.0; // Δψ, degrees
    double obliquity = 0.0; // Δε, degrees
};
[[nodiscard]] Nutation nutation(double T);

// Greenwich mean sidereal time (Meeus 12.4), from days of UT since J2000.0.
[[nodiscard]] double greenwich_mean_sidereal_time(double days_ut);

struct Equatorial {
    double right_ascension = 0.0; // degrees, [0, 360)
    double declination = 0.0;     // degrees
};
struct Horizontal {
    double azimuth = 0.0;   // compass bearing, degrees, [0, 360)
    double elevation = 0.0; // degrees above the horizon
};

// Ecliptic longitude / latitude -> equatorial, for obliquity `eps` (13.3, 13.4).
[[nodiscard]] Equatorial equatorial_from_ecliptic(double longitude, double latitude, double eps);
// Unit vector of equatorial coordinates (x toward the equinox, z to the pole).
[[nodiscard]] glm::dvec3 equatorial_vector(const Equatorial& eq);
[[nodiscard]] Equatorial equatorial_from_vector(const glm::dvec3& v);

// Rotation taking equatorial vectors of the epoch of date into the observer's
// ENU frame, for local sidereal time `lst` and geodetic latitude `latitude`.
// Hour angle H = lst − α (Meeus 13.5, 13.6 as a matrix).
[[nodiscard]] glm::dmat3 enu_from_equatorial(double lst, double latitude);
[[nodiscard]] Horizontal horizontal_from_enu(const glm::dvec3& enu);
[[nodiscard]] glm::dvec3 enu_from_horizontal(const Horizontal& h);
// α, δ seen at local sidereal time `lst` and latitude -> azimuth, elevation.
[[nodiscard]] Horizontal horizontal_from_equatorial(const Equatorial& eq, double lst, double latitude);

// Precession of equatorial vectors from J2000.0 to the mean equator and
// equinox of date (IAU 1976: ζ, z, θ; Meeus 21.2–21.4), T in centuries TT.
[[nodiscard]] glm::dmat3 precession_from_j2000(double T);

// The observer's position relative to Earth's centre in its ENU frame, km,
// on the IAU 1976 ellipsoid at sea level (Meeus ch. 11: ρ sin φ', ρ cos φ').
[[nodiscard]] glm::dvec3 observer_offset_enu(double latitude);
// Topocentric unit direction (ENU) of a body at `distance_km` whose
// geocentric direction is `geocentric_enu` (diurnal parallax, exact
// vector form of Meeus ch. 40).
[[nodiscard]] glm::dvec3 topocentric_direction(const glm::dvec3& geocentric_enu, double distance_km,
                                               double latitude);

// Atmospheric refraction for a true (airless) elevation `h`, degrees, at
// 1010 hPa and 10 °C (Sæmundsson 1986, Meeus 16.4). Below −1° the value
// tapers linearly to 0 at −5°, keeping the apparent elevation continuous and
// monotonic where refraction has no physical meaning (no line of sight).
[[nodiscard]] double refraction(double true_elevation);
[[nodiscard]] inline double apparent_elevation(double true_elevation) {
    return true_elevation + refraction(true_elevation);
}

// Relative optical airmass for an apparent elevation (Kasten & Young 1989):
// 1 at the zenith, ~38 at the horizon. Below the horizon the formula has no
// meaning (there is no direct path); the horizon value is returned.
[[nodiscard]] double airmass(double apparent_elevation);
// Light left after `airmass` with an extinction of `k` magnitudes per
// airmass: 10^(-0.4 k X).
[[nodiscard]] double extinction_factor(double airmass, double k_mag_per_airmass);

// The engine's world axes (see the header comment): columns are the world
// directions of east, north and up, so world = M * enu.
[[nodiscard]] glm::dmat3 world_from_enu(double north_offset_deg);

} // namespace engine::astro
