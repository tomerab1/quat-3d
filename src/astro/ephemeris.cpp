// Topocentric Sun and Moon — see ephemeris.hpp.

#include "ephemeris.hpp"

#include <cmath>

namespace engine::astro::detail {

Ephemeris ephemeris(UtcTime t, double latitude, double longitude, bool with_moon) {
    Ephemeris e;
    e.days_ut = days_ut_since_j2000(t);
    e.days_tt = days_tt_since_j2000(t);
    const double   T = centuries_tt(e.days_tt);
    const Nutation n = nutation(T);
    const double   eps = mean_obliquity(T) + n.obliquity;
    // Equation of the equinoxes: apparent = mean + Δψ cos ε (Meeus ch. 12).
    const double gmst = greenwich_mean_sidereal_time(e.days_ut);
    e.lst_mean = wrap_360(gmst + longitude);
    e.lst_apparent = wrap_360(gmst + n.longitude * std::cos(eps * k_deg) + longitude);
    e.enu_from_equatorial = enu_from_equatorial(e.lst_apparent, latitude);

    e.sun = sun_position(e.days_tt);
    e.sun_enu = topocentric_direction(e.enu_from_equatorial * equatorial_vector(e.sun.equatorial),
                                      e.sun.distance_au * k_km_per_au, latitude);
    if (with_moon) {
        e.has_moon = true;
        e.moon = moon_position(e.days_tt);
        const glm::dvec3 geo = e.enu_from_equatorial * equatorial_vector(e.moon.equatorial);
        const glm::dvec3 topo = geo * e.moon.distance_km - observer_offset_enu(latitude);
        e.moon_topo_distance_km = glm::length(topo);
        e.moon_enu = topo / e.moon_topo_distance_km;
    }
    return e;
}

} // namespace engine::astro::detail
