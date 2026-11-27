#pragma once

// The Sun's apparent geocentric position (ROADMAP R22.1): Meeus,
// "Astronomical Algorithms", 2nd ed., ch. 25 "lower accuracy" method — the
// same algorithm NOAA's Solar Calculator uses — good to about 0.01°, plus the
// equation of time (Meeus 28.3, Smart's formula). Rising, setting and
// transit are in events.hpp.

#include "engine/astro/coords.hpp"

namespace engine::astro {

struct SunPosition {
    double     longitude = 0.0;    // apparent ecliptic longitude λ (nutation + aberration), degrees
    double     distance_au = 1.0;  // R, astronomical units
    double     true_obliquity = 0.0; // ε used for the conversion, degrees
    Equatorial equatorial;         // apparent α, δ of date
    double     equation_of_time_min = 0.0; // apparent − mean solar time, minutes
};

// Position at `days_tt` days of TT since J2000.0.
[[nodiscard]] SunPosition sun_position(double days_tt);

// Sun's angular radius (degrees) at `distance_au` (959.63" at 1 AU).
[[nodiscard]] double sun_angular_radius(double distance_au);

// Extra-atmospheric solar illuminance at normal incidence, lux, at
// `distance_au`: the luminous solar constant 133 334 lx (Darula, Kittler &
// Gueymard, "Reference luminous solar constant and solar luminance for
// illuminance calculations", Solar Energy 79, 2005) scaled by 1/R².
[[nodiscard]] double sun_illuminance_lux(double distance_au);

} // namespace engine::astro
