#pragma once

// The Moon (ROADMAP R22.1): apparent geocentric position from the truncated
// ELP-2000/82 series of Meeus, "Astronomical Algorithms", 2nd ed., ch. 47
// (≈ 10" in longitude, 4" in latitude); phase angle, illuminated fraction and
// the position angle of the bright limb (ch. 48); optical libration and the
// position angle of the axis (ch. 53, physical libration — at most 0.04° —
// omitted); and the extra-atmospheric illuminance it casts on Earth
// (Krisciunas & Schaefer, "A model of the brightness of moonlight",
// PASP 103, 1991, eq. 20).

#include "engine/astro/coords.hpp"
#include "engine/astro/sun.hpp"

namespace engine::astro {

inline constexpr double k_moon_radius_km = 1737.4;
inline constexpr double k_moon_mean_distance_km = 384400.0;

struct MoonPosition {
    double     longitude = 0.0;   // apparent ecliptic longitude λ (incl. nutation), degrees
    double     latitude = 0.0;    // ecliptic latitude β, degrees
    double     distance_km = 0.0; // Δ, Earth centre to Moon centre
    double     parallax = 0.0;    // equatorial horizontal parallax π, degrees
    double     true_obliquity = 0.0;
    Equatorial equatorial;        // apparent geocentric α, δ of date
    // Arguments the libration needs (degrees).
    double argument_of_latitude = 0.0; // F
    double ascending_node = 0.0;       // Ω of the mean orbit
    double nutation_longitude = 0.0;   // Δψ
};

// Position at `days_tt` days of TT since J2000.0.
[[nodiscard]] MoonPosition moon_position(double days_tt);

// Sums of the periodic terms (Meeus Σl, Σb, Σr: 1e-6 degrees and 1e-3 km),
// exposed so tests can check them against Example 47.a.
struct MoonSeriesSums {
    double l = 0.0;
    double b = 0.0;
    double r = 0.0;
};
[[nodiscard]] MoonSeriesSums moon_series_sums(double days_tt);

struct MoonPhase {
    double phase_angle = 0.0;          // i: Sun–Moon–Earth angle, degrees (0 = full)
    double illuminated_fraction = 0.0; // k = (1 + cos i) / 2
    double bright_limb = 0.0;          // χ: position angle of the bright limb's midpoint, from celestial north through east
    double elongation = 0.0;           // ψ: geocentric Sun–Moon angle, degrees
    bool   waxing = true;              // the Moon's longitude leads the Sun's by < 180°
};
[[nodiscard]] MoonPhase moon_phase(const MoonPosition& moon, const SunPosition& sun);

// The conventional name of the phase ("new", "waxing crescent", "first
// quarter", "waxing gibbous", "full", "waning gibbous", "last quarter",
// "waning crescent"): eight 45° sectors of the elongation, centred on the
// principal phases.
[[nodiscard]] const char* moon_phase_name(double elongation_deg, bool waxing);

struct MoonLibration {
    double longitude = 0.0;      // l': selenographic longitude of the sub-Earth point, degrees
    double latitude = 0.0;       // b': its latitude, degrees
    double position_angle = 0.0; // P: of the Moon's north pole, from celestial north through east
};
[[nodiscard]] MoonLibration moon_libration(const MoonPosition& moon);

// Selenographic -> equatorial-of-date rotation (columns: the Moon's body
// axes x = 0°N 0°E, y = 0°N 90°E, z = north pole) for a Moon whose
// geocentric unit direction is `direction` (equatorial of date) with
// libration `lib`: the sub-Earth point lands at (l', b') and the projected
// pole at position angle P.
[[nodiscard]] glm::dmat3 moon_body_axes(const glm::dvec3& direction, const MoonLibration& lib);

// Angular radius (degrees) seen from `distance_km`.
[[nodiscard]] double moon_angular_radius(double distance_km);

// Extra-atmospheric illuminance (lux, normal incidence) from a Moon at
// `phase_angle_deg` and `distance_km` (Krisciunas & Schaefer 1991, eq. 20,
// with the inverse-square distance scaling): ~0.31 lx full at mean distance.
[[nodiscard]] double moon_illuminance_lux(double phase_angle_deg, double distance_km);

} // namespace engine::astro
