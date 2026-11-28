#pragma once

// Rising, setting and transit of the Sun and Moon, and twilight (ROADMAP
// R22.1), for an observer at sea level.
//
// The topocentric altitude of the body's centre is sampled every 10 minutes
// across the window and each crossing of the event altitude is refined by
// bisection to < 0.1 s on the full ephemeris — the scan-and-refine approach
// of Montenbruck & Pfleger, "Astronomy on the Personal Computer" (4th ed.,
// 2000, sec. 3.8), with a finer step. Excursions shorter than a step (the
// first and last days of polar day / night, grazing high-latitude moonrises)
// leave no sign change between samples: wherever three samples bend toward
// the event altitude and come within 1° of it, the extremum between them is
// located (golden-section search) and its sign decides whether the body
// briefly crossed. Event altitudes follow the USNO / Meeus (ch. 15)
// conventions:
//   - sunrise / sunset: upper limb on the horizon with 34' of refraction,
//     h0 = −0°50' (semidiameter 16' + refraction 34').
//   - moonrise / moonset: upper limb on the horizon, topocentric, with 34' of
//     refraction (h0 = −34' − the Moon's topocentric semidiameter).
//   - civil / nautical / astronomical twilight: the Sun's centre at −6° / −12°
//     / −18°.
// Transit is the upper meridian passage (local apparent hour angle 0).

#include <optional>

#include "engine/astro/time.hpp"

namespace engine::astro {

inline constexpr double k_sunrise_altitude = -50.0 / 60.0;
inline constexpr double k_civil_twilight_altitude = -6.0;
inline constexpr double k_nautical_twilight_altitude = -12.0;
inline constexpr double k_astronomical_twilight_altitude = -18.0;

enum class Body { sun, moon };

struct RiseSet {
    std::optional<UtcTime> rise;    // first upward crossing in the window
    std::optional<UtcTime> set;     // first downward crossing in the window
    std::optional<UtcTime> transit; // upper meridian passage
    bool always_above = false;      // no crossing, above the event altitude throughout
    bool always_below = false;      // no crossing, below throughout
};

// Events of `body` in [start, start + 24 h). For the Sun `altitude` picks the
// event (k_sunrise_altitude, or a twilight altitude); the Moon always uses
// its own (topocentric upper limb) definition and ignores it.
[[nodiscard]] RiseSet rise_set(Body body, UtcTime start, double latitude, double longitude,
                               double altitude = k_sunrise_altitude);

// The start of the local calendar day (00:00 at UTC + `utc_offset_min`)
// containing `t`.
[[nodiscard]] UtcTime local_midnight(UtcTime t, int utc_offset_min);

} // namespace engine::astro
