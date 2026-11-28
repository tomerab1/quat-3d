#pragma once

// Sun and Moon as seen by one observer at one instant, in the observer's
// ENU frame (topocentric, airless). Private to src/astro: events.cpp and
// sky_state.cpp share it.

#include <glm/glm.hpp>

#include "engine/astro/moon.hpp"
#include "engine/astro/sun.hpp"
#include "engine/astro/time.hpp"

namespace engine::astro::detail {

struct Ephemeris {
    double days_ut = 0.0;
    double days_tt = 0.0;
    double lst_mean = 0.0;     // local mean sidereal time, degrees
    double lst_apparent = 0.0; // local apparent sidereal time (with the equation of the equinoxes)
    glm::dmat3 enu_from_equatorial{1.0}; // apparent equator of date, at lst_apparent

    SunPosition sun;
    glm::dvec3  sun_enu{0.0, 0.0, 1.0}; // topocentric unit direction

    bool         has_moon = false;
    MoonPosition moon;
    glm::dvec3   moon_enu{0.0, 0.0, 1.0}; // topocentric unit direction
    double       moon_topo_distance_km = 0.0;
};

// `with_moon` false skips the Moon's series (sunrise searches).
[[nodiscard]] Ephemeris ephemeris(UtcTime t, double latitude, double longitude, bool with_moon);

} // namespace engine::astro::detail
