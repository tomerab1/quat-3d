// Coordinate systems and Earth rotation — see include/engine/astro/coords.hpp.

#include "engine/astro/coords.hpp"

#include <algorithm>
#include <cmath>

namespace engine::astro {

namespace {

constexpr double k_arcsec = 1.0 / 3600.0;

// A matrix from its rows (glm is column-major).
[[nodiscard]] glm::dmat3 from_rows(const glm::dvec3& r0, const glm::dvec3& r1, const glm::dvec3& r2) {
    return glm::transpose(glm::dmat3(r0, r1, r2));
}

// Active rotations (rotate a vector by `deg` about the axis, right-handed).
[[nodiscard]] glm::dmat3 rot_z(double deg) {
    const double c = std::cos(deg * k_deg);
    const double s = std::sin(deg * k_deg);
    return from_rows({c, -s, 0.0}, {s, c, 0.0}, {0.0, 0.0, 1.0});
}
[[nodiscard]] glm::dmat3 rot_y(double deg) {
    const double c = std::cos(deg * k_deg);
    const double s = std::sin(deg * k_deg);
    return from_rows({c, 0.0, s}, {0.0, 1.0, 0.0}, {-s, 0.0, c});
}

} // namespace

double wrap_360(double deg) {
    double r = std::fmod(deg, 360.0);
    if (r < 0.0) r += 360.0;
    return r >= 360.0 ? 0.0 : r;
}

double wrap_180(double deg) {
    double r = wrap_360(deg);
    return r > 180.0 ? r - 360.0 : r;
}

double mean_obliquity(double T) {
    // 23°26'21.448" − 46.8150"T − 0.00059"T² + 0.001813"T³ (Meeus 22.2)
    return 23.0 + 26.0 / 60.0 +
           (21.448 - T * (46.8150 + T * (0.00059 - T * 0.001813))) * k_arcsec;
}

Nutation nutation(double T) {
    const double omega = (125.04452 - 1934.136261 * T + 0.0020708 * T * T + T * T * T / 450000.0) * k_deg;
    const double L = (280.4665 + 36000.7698 * T) * k_deg;  // Sun's mean longitude
    const double Lm = (218.3165 + 481267.8813 * T) * k_deg; // Moon's mean longitude
    Nutation n;
    n.longitude = (-17.20 * std::sin(omega) - 1.32 * std::sin(2.0 * L) - 0.23 * std::sin(2.0 * Lm) +
                   0.21 * std::sin(2.0 * omega)) *
                  k_arcsec;
    n.obliquity = (9.20 * std::cos(omega) + 0.57 * std::cos(2.0 * L) + 0.10 * std::cos(2.0 * Lm) -
                   0.09 * std::cos(2.0 * omega)) *
                  k_arcsec;
    return n;
}

double greenwich_mean_sidereal_time(double days_ut) {
    const double T = days_ut / 36525.0;
    // Meeus 12.4: the whole-revolution part of 360.98564736629 d is split
    // off first so the sum keeps its precision.
    const double revolutions = 0.98564736629 * days_ut;
    return wrap_360(280.46061837 + wrap_360(360.0 * std::fmod(days_ut, 1.0)) + revolutions +
                    T * T * (0.000387933 - T / 38710000.0));
}

Equatorial equatorial_from_ecliptic(double longitude, double latitude, double eps) {
    const double l = longitude * k_deg;
    const double b = latitude * k_deg;
    const double e = eps * k_deg;
    Equatorial   q;
    q.right_ascension =
        wrap_360(std::atan2(std::sin(l) * std::cos(e) - std::tan(b) * std::sin(e), std::cos(l)) / k_deg);
    q.declination =
        std::asin(std::clamp(std::sin(b) * std::cos(e) + std::cos(b) * std::sin(e) * std::sin(l), -1.0, 1.0)) /
        k_deg;
    return q;
}

glm::dvec3 equatorial_vector(const Equatorial& eq) {
    const double a = eq.right_ascension * k_deg;
    const double d = eq.declination * k_deg;
    return {std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d)};
}

Equatorial equatorial_from_vector(const glm::dvec3& v) {
    const glm::dvec3 u = glm::normalize(v);
    return {wrap_360(std::atan2(u.y, u.x) / k_deg), std::asin(std::clamp(u.z, -1.0, 1.0)) / k_deg};
}

glm::dmat3 enu_from_equatorial(double lst, double latitude) {
    // Rotate the hour circle of `lst` onto the meridian (x' = cos δ cos H,
    // y' = −cos δ sin H = east, z' = pole), then tip the pole down to the
    // observer's latitude.
    const glm::dmat3 to_meridian = rot_z(-lst);
    const double     sp = std::sin(latitude * k_deg);
    const double     cp = std::cos(latitude * k_deg);
    const glm::dmat3 to_enu = from_rows({0.0, 1.0, 0.0}, {-sp, 0.0, cp}, {cp, 0.0, sp});
    return to_enu * to_meridian;
}

Horizontal horizontal_from_enu(const glm::dvec3& enu) {
    const glm::dvec3 u = glm::normalize(enu);
    return {wrap_360(std::atan2(u.x, u.y) / k_deg), std::asin(std::clamp(u.z, -1.0, 1.0)) / k_deg};
}

glm::dvec3 enu_from_horizontal(const Horizontal& h) {
    const double a = h.azimuth * k_deg;
    const double e = h.elevation * k_deg;
    return {std::cos(e) * std::sin(a), std::cos(e) * std::cos(a), std::sin(e)};
}

Horizontal horizontal_from_equatorial(const Equatorial& eq, double lst, double latitude) {
    return horizontal_from_enu(enu_from_equatorial(lst, latitude) * equatorial_vector(eq));
}

glm::dmat3 precession_from_j2000(double T) {
    const double zeta = (2306.2181 * T + 0.30188 * T * T + 0.017998 * T * T * T) * k_arcsec;
    const double z = (2306.2181 * T + 1.09468 * T * T + 0.018203 * T * T * T) * k_arcsec;
    const double theta = (2004.3109 * T - 0.42665 * T * T - 0.041833 * T * T * T) * k_arcsec;
    // α = z + atan2(A, B) with A, B, C from α0 + ζ (Meeus 21.4): rotate by
    // ζ about the pole, by −θ about y, then by z about the new pole.
    return rot_z(z) * rot_y(-theta) * rot_z(zeta);
}

glm::dvec3 observer_offset_enu(double latitude) {
    constexpr double k_b_over_a = 0.99664719; // IAU 1976 ellipsoid
    const double     phi = latitude * k_deg;
    const double     u = std::atan(k_b_over_a * std::tan(phi));
    const double     rho_sin = k_b_over_a * std::sin(u); // ρ sin φ'
    const double     rho_cos = std::cos(u);              // ρ cos φ'
    // (equatorial-radial, polar) components rotated into (north, up).
    const double north = -rho_cos * std::sin(phi) + rho_sin * std::cos(phi);
    const double up = rho_cos * std::cos(phi) + rho_sin * std::sin(phi);
    return glm::dvec3(0.0, north, up) * k_earth_equatorial_radius_km;
}

glm::dvec3 topocentric_direction(const glm::dvec3& geocentric_enu, double distance_km, double latitude) {
    return glm::normalize(glm::normalize(geocentric_enu) * distance_km - observer_offset_enu(latitude));
}

double refraction(double true_elevation) {
    const auto saemundsson = [](double h) {
        // R in arcminutes; +0.0019279 makes R(90°) = 0 (Meeus p. 106).
        const double r_arcmin = 1.02 / std::tan((h + 10.3 / (h + 5.11)) * k_deg) + 0.0019279;
        return std::max(r_arcmin, 0.0) / 60.0;
    };
    if (true_elevation >= -1.0) return saemundsson(true_elevation);
    return saemundsson(-1.0) * std::max(0.0, (true_elevation + 5.0) / 4.0);
}

double airmass(double apparent_elevation) {
    const double h = std::max(apparent_elevation, 0.0);
    return 1.0 / (std::sin(h * k_deg) + 0.50572 * std::pow(h + 6.07995, -1.6364));
}

double extinction_factor(double airmass_value, double k_mag_per_airmass) {
    return std::pow(10.0, -0.4 * k_mag_per_airmass * airmass_value);
}

glm::dmat3 world_from_enu(double north_offset_deg) {
    const double     s = std::sin(north_offset_deg * k_deg);
    const double     c = std::cos(north_offset_deg * k_deg);
    const glm::dvec3 east(c, 0.0, s);
    const glm::dvec3 north(s, 0.0, -c);
    const glm::dvec3 up(0.0, 1.0, 0.0);
    return glm::dmat3(east, north, up);
}

} // namespace engine::astro
