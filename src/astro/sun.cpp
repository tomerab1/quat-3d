// The Sun's apparent position — see include/engine/astro/sun.hpp.

#include "engine/astro/sun.hpp"

#include <cmath>

namespace engine::astro {

SunPosition sun_position(double days_tt) {
    const double T = days_tt / 36525.0;
    // Meeus 25.2–25.4: geometric mean longitude, mean anomaly, eccentricity.
    const double L0 = wrap_360(280.46646 + T * (36000.76983 + T * 0.0003032));
    const double M = wrap_360(357.52911 + T * (35999.05029 - T * 0.0001537));
    const double e = 0.016708634 - T * (0.000042037 + T * 0.0000001267);
    const double Mr = M * k_deg;
    // Equation of the centre.
    const double C = (1.914602 - T * (0.004817 + T * 0.000014)) * std::sin(Mr) +
                     (0.019993 - T * 0.000101) * std::sin(2.0 * Mr) + 0.000289 * std::sin(3.0 * Mr);
    const double true_longitude = L0 + C;
    const double nu = (M + C) * k_deg;
    const double R = 1.000001018 * (1.0 - e * e) / (1.0 + e * std::cos(nu)); // 25.5
    // Apparent longitude: nutation and aberration through Ω (25.6 and p. 165).
    const double omega = (125.04 - 1934.136 * T) * k_deg;
    const double lambda = true_longitude - 0.00569 - 0.00478 * std::sin(omega);
    const double eps = mean_obliquity(T) + 0.00256 * std::cos(omega); // 25.8

    SunPosition s;
    s.longitude = wrap_360(lambda);
    s.distance_au = R;
    s.true_obliquity = eps;
    s.equatorial = equatorial_from_ecliptic(lambda, 0.0, eps);

    // Equation of time (Meeus 28.3, Smart 1956), in radians -> minutes.
    const double y = std::tan(eps * k_deg / 2.0) * std::tan(eps * k_deg / 2.0);
    const double l0 = L0 * k_deg;
    const double E = y * std::sin(2.0 * l0) - 2.0 * e * std::sin(Mr) +
                     4.0 * e * y * std::sin(Mr) * std::cos(2.0 * l0) -
                     0.5 * y * y * std::sin(4.0 * l0) - 1.25 * e * e * std::sin(2.0 * Mr);
    s.equation_of_time_min = E / k_deg * 4.0;
    return s;
}

double sun_angular_radius(double distance_au) { return 959.63 / 3600.0 / distance_au; }

double sun_illuminance_lux(double distance_au) {
    constexpr double k_luminous_solar_constant = 133334.0; // lx at 1 AU
    return k_luminous_solar_constant / (distance_au * distance_au);
}

} // namespace engine::astro
