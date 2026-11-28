// The Moon — see include/engine/astro/moon.hpp.

#include "engine/astro/moon.hpp"

#include <algorithm>
#include <cmath>

#include "moon_terms.hpp"

namespace engine::astro {

namespace {

// Fundamental arguments (Meeus 47.1–47.7, 47.6), degrees.
struct MoonArguments {
    double Lp = 0.0; // L': mean longitude
    double D = 0.0;  // mean elongation
    double M = 0.0;  // Sun's mean anomaly
    double Mp = 0.0; // M': Moon's mean anomaly
    double F = 0.0;  // argument of latitude
    double A1 = 0.0;
    double A2 = 0.0;
    double A3 = 0.0;
    double E = 1.0; // eccentricity factor of Earth's orbit
};

[[nodiscard]] MoonArguments arguments(double T) {
    const double T2 = T * T;
    const double T3 = T2 * T;
    const double T4 = T3 * T;
    MoonArguments a;
    a.Lp = wrap_360(218.3164477 + 481267.88123421 * T - 0.0015786 * T2 + T3 / 538841.0 - T4 / 65194000.0);
    a.D = wrap_360(297.8501921 + 445267.1114034 * T - 0.0018819 * T2 + T3 / 545868.0 - T4 / 113065000.0);
    a.M = wrap_360(357.5291092 + 35999.0502909 * T - 0.0001536 * T2 + T3 / 24490000.0);
    a.Mp = wrap_360(134.9633964 + 477198.8675055 * T + 0.0087414 * T2 + T3 / 69699.0 - T4 / 14712000.0);
    a.F = wrap_360(93.2720950 + 483202.0175233 * T - 0.0036539 * T2 - T3 / 3526000.0 + T4 / 863310000.0);
    a.A1 = wrap_360(119.75 + 131.849 * T);
    a.A2 = wrap_360(53.09 + 479264.290 * T);
    a.A3 = wrap_360(313.45 + 481266.484 * T);
    a.E = 1.0 - 0.002516 * T - 0.0000074 * T2;
    return a;
}

// Terms with M carry E^|multiple of M| (the decreasing eccentricity).
[[nodiscard]] double eccentricity_factor(int m, double E) {
    const int am = m < 0 ? -m : m;
    return am == 1 ? E : (am == 2 ? E * E : 1.0);
}

[[nodiscard]] MoonSeriesSums sums(const MoonArguments& a) {
    MoonSeriesSums s;
    for (const detail::MoonLrTerm& t : detail::k_moon_lr_terms) {
        const double arg = (t.d * a.D + t.m * a.M + t.mp * a.Mp + t.f * a.F) * k_deg;
        const double e = eccentricity_factor(t.m, a.E);
        s.l += t.l * e * std::sin(arg);
        s.r += t.r * e * std::cos(arg);
    }
    for (const detail::MoonBTerm& t : detail::k_moon_b_terms) {
        const double arg = (t.d * a.D + t.m * a.M + t.mp * a.Mp + t.f * a.F) * k_deg;
        s.b += t.b * eccentricity_factor(t.m, a.E) * std::sin(arg);
    }
    // Additive terms: Venus (A1), Jupiter (A2) and Earth's flattening (L').
    const double r = k_deg;
    s.l += 3958.0 * std::sin(a.A1 * r) + 1962.0 * std::sin((a.Lp - a.F) * r) + 318.0 * std::sin(a.A2 * r);
    s.b += -2235.0 * std::sin(a.Lp * r) + 382.0 * std::sin(a.A3 * r) + 175.0 * std::sin((a.A1 - a.F) * r) +
           175.0 * std::sin((a.A1 + a.F) * r) + 127.0 * std::sin((a.Lp - a.Mp) * r) -
           115.0 * std::sin((a.Lp + a.Mp) * r);
    return s;
}

} // namespace

MoonSeriesSums moon_series_sums(double days_tt) { return sums(arguments(days_tt / 36525.0)); }

MoonPosition moon_position(double days_tt) {
    const double        T = days_tt / 36525.0;
    const MoonArguments a = arguments(T);
    const MoonSeriesSums s = sums(a);
    const Nutation       n = nutation(T);

    MoonPosition m;
    m.nutation_longitude = n.longitude;
    m.longitude = wrap_360(a.Lp + s.l * 1e-6 + n.longitude);
    m.latitude = s.b * 1e-6;
    m.distance_km = 385000.56 + s.r * 1e-3;
    m.parallax = std::asin(k_earth_equatorial_radius_km / m.distance_km) / k_deg;
    m.true_obliquity = mean_obliquity(T) + n.obliquity;
    m.equatorial = equatorial_from_ecliptic(m.longitude, m.latitude, m.true_obliquity);
    m.argument_of_latitude = a.F;
    // Mean ascending node (Meeus 47.7).
    m.ascending_node = wrap_360(125.0445479 - 1934.1362891 * T + 0.0020754 * T * T +
                                T * T * T / 467441.0 - T * T * T * T / 60616000.0);
    return m;
}

MoonPhase moon_phase(const MoonPosition& moon, const SunPosition& sun) {
    const double a0 = sun.equatorial.right_ascension * k_deg;
    const double d0 = sun.equatorial.declination * k_deg;
    const double a = moon.equatorial.right_ascension * k_deg;
    const double d = moon.equatorial.declination * k_deg;
    // Geocentric elongation ψ (48.2) and phase angle i (48.3).
    const double cos_psi = std::clamp(
        std::sin(d0) * std::sin(d) + std::cos(d0) * std::cos(d) * std::cos(a0 - a), -1.0, 1.0);
    const double psi = std::acos(cos_psi);
    const double R = sun.distance_au * k_km_per_au;
    const double i = std::atan2(R * std::sin(psi), moon.distance_km - R * std::cos(psi));

    MoonPhase p;
    p.elongation = psi / k_deg;
    p.phase_angle = i / k_deg;
    p.illuminated_fraction = (1.0 + std::cos(i)) / 2.0; // 48.1
    // Position angle of the bright limb (48.5).
    p.bright_limb = wrap_360(std::atan2(std::cos(d0) * std::sin(a0 - a),
                                        std::sin(d0) * std::cos(d) -
                                            std::cos(d0) * std::sin(d) * std::cos(a0 - a)) /
                             k_deg);
    p.waxing = wrap_360(moon.longitude - sun.longitude) < 180.0;
    return p;
}

const char* moon_phase_name(double elongation_deg, bool waxing) {
    // Age as an angle 0..360 (0 new, 180 full), then the nearest of eight.
    const double age = waxing ? elongation_deg : 360.0 - elongation_deg;
    static constexpr const char* k_names[] = {"new",  "waxing crescent", "first quarter", "waxing gibbous",
                                              "full", "waning gibbous",  "last quarter",  "waning crescent"};
    const auto sector = static_cast<int>(std::floor(wrap_360(age + 22.5) / 45.0)) % 8;
    return k_names[sector];
}

MoonLibration moon_libration(const MoonPosition& moon) {
    constexpr double k_inclination = 1.54242; // I: lunar equator to ecliptic
    const double     I = k_inclination * k_deg;
    const double     beta = moon.latitude * k_deg;
    // W = λ − Δψ − Ω with λ the apparent longitude (Meeus 53.1).
    const double W = (moon.longitude - moon.nutation_longitude - moon.ascending_node) * k_deg;
    const double A = std::atan2(std::sin(W) * std::cos(beta) * std::cos(I) - std::sin(beta) * std::sin(I),
                                std::cos(W) * std::cos(beta));

    MoonLibration lib;
    lib.longitude = wrap_180(A / k_deg - moon.argument_of_latitude);
    const double b = std::asin(std::clamp(
        -std::sin(W) * std::cos(beta) * std::sin(I) - std::sin(beta) * std::cos(I), -1.0, 1.0));
    lib.latitude = b / k_deg;

    // Position angle of the axis (Meeus 53.2 with ρ = σ = 0).
    const double V = (moon.ascending_node + moon.nutation_longitude) * k_deg;
    const double eps = moon.true_obliquity * k_deg;
    const double X = std::sin(I) * std::sin(V);
    const double Y = std::sin(I) * std::cos(V) * std::cos(eps) - std::cos(I) * std::sin(eps);
    const double omega = std::atan2(X, Y);
    const double alpha = moon.equatorial.right_ascension * k_deg;
    const double sin_p = std::sqrt(X * X + Y * Y) * std::cos(alpha - omega) / std::cos(b);
    lib.position_angle = wrap_360(std::asin(std::clamp(sin_p, -1.0, 1.0)) / k_deg);
    return lib;
}

glm::dmat3 moon_body_axes(const glm::dvec3& direction, const MoonLibration& lib) {
    const glm::dvec3 d = glm::normalize(direction);
    const glm::dvec3 pole(0.0, 0.0, 1.0);
    const glm::dvec3 to_earth = -d;
    // On the sky: north toward the celestial pole, east toward increasing
    // right ascension; P is measured from north through east.
    const glm::dvec3 sky_north = glm::normalize(pole - glm::dot(pole, d) * d);
    const glm::dvec3 sky_east = glm::normalize(glm::cross(pole, d));
    const double     P = lib.position_angle * k_deg;
    const double     b = lib.latitude * k_deg;
    const double     l = lib.longitude * k_deg;
    const glm::dvec3 axis_on_sky = std::cos(P) * sky_north + std::sin(P) * sky_east;
    // The pole leans toward Earth by the sub-Earth latitude b.
    const glm::dvec3 z = glm::normalize(std::sin(b) * to_earth + std::cos(b) * axis_on_sky);
    // The sub-Earth direction projected on the lunar equator sits at
    // longitude l; turn it back by l to get the prime meridian.
    const glm::dvec3 sub_earth = glm::normalize(to_earth - glm::dot(to_earth, z) * z);
    const glm::dvec3 x = std::cos(l) * sub_earth - std::sin(l) * glm::cross(z, sub_earth);
    const glm::dvec3 y = glm::cross(z, x);
    return glm::dmat3(x, y, z);
}

double moon_angular_radius(double distance_km) {
    return std::asin(std::clamp(k_moon_radius_km / distance_km, 0.0, 1.0)) / k_deg;
}

double moon_illuminance_lux(double phase_angle_deg, double distance_km) {
    // Krisciunas & Schaefer (1991) eq. 20: I* in foot-candles at the mean
    // distance; 1 fc = 10.7639 lx.
    constexpr double k_lux_per_footcandle = 10.7639;
    const double     a = std::abs(phase_angle_deg);
    const double     fc = std::pow(10.0, -0.4 * (3.84 + 0.026 * a + 4.0e-9 * a * a * a * a));
    const double     scale = k_moon_mean_distance_km / distance_km;
    return fc * k_lux_per_footcandle * scale * scale;
}

} // namespace engine::astro
