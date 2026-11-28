#pragma once

// Helpers for the astronomy tests: sexagesimal values as printed in the
// references, and instants from calendar fields.

#include "engine/astro/time.hpp"

namespace quat::test {

// h m s of right ascension / sidereal time -> degrees.
[[nodiscard]] constexpr double hms(double h, double m, double s) {
    return (h + m / 60.0 + s / 3600.0) * 15.0;
}
// ±d m s -> degrees (the sign applies to the whole value).
[[nodiscard]] constexpr double dms(double d, double m, double s, bool negative = false) {
    const double v = d + m / 60.0 + s / 3600.0;
    return negative ? -v : v;
}

[[nodiscard]] inline engine::astro::UtcTime utc(int y, int mo, int d, int h = 0, int mi = 0,
                                                double s = 0.0) {
    return engine::astro::utc_from_civil({y, mo, d, h, mi, s}, 0);
}

// Days of TT since J2000.0 for a Julian Ephemeris Day as the examples give it.
[[nodiscard]] constexpr double days_from_jde(double jde) { return jde - engine::astro::k_jd_j2000; }

} // namespace quat::test
