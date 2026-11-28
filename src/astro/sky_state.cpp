// SkyState — see include/engine/astro/sky_state.hpp.

#include "engine/astro/sky_state.hpp"

#include <algorithm>
#include <cmath>

#include "engine/astro/coords.hpp"
#include "engine/astro/moon.hpp"
#include "engine/astro/sun.hpp"
#include "ephemeris.hpp"

namespace engine::astro {

namespace {

// A topocentric airless ENU direction lifted by refraction to where it is
// seen.
struct Apparent {
    Horizontal horizontal;
    glm::dvec3 enu{0.0};
};
[[nodiscard]] Apparent apparent(const glm::dvec3& true_enu) {
    Apparent a;
    a.horizontal = horizontal_from_enu(true_enu);
    a.horizontal.elevation = apparent_elevation(a.horizontal.elevation);
    a.enu = enu_from_horizontal(a.horizontal);
    return a;
}

} // namespace

double disc_fraction_above_horizon(double elevation, double radius) {
    if (radius <= 0.0) return elevation > 0.0 ? 1.0 : 0.0;
    const double x = std::clamp(elevation / radius, -1.0, 1.0);
    // Unit-disc area below the chord at height −x (circular segment).
    const double below = std::acos(x) - x * std::sqrt(1.0 - x * x);
    return std::clamp(1.0 - below / k_pi, 0.0, 1.0);
}

SkyState compute_sky_state(UtcTime t, const Observer& o) {
    const detail::Ephemeris e = detail::ephemeris(t, o.latitude, o.longitude, true);
    const glm::dmat3        world_enu = world_from_enu(o.north_offset_deg);
    const glm::dmat3        world_eq = world_enu * e.enu_from_equatorial; // apparent equator of date

    SkyState s;
    s.julian_date_ut = k_jd_j2000 + e.days_ut;
    s.local_sidereal_time_deg = e.lst_mean;

    // Sun.
    const Apparent sun = apparent(e.sun_enu);
    s.sun_direction = glm::vec3(glm::normalize(world_enu * sun.enu));
    s.sun_azimuth_deg = static_cast<float>(sun.horizontal.azimuth);
    s.sun_elevation_deg = static_cast<float>(sun.horizontal.elevation);
    const double sun_radius = sun_angular_radius(e.sun.distance_au);
    s.sun_angular_radius_deg = static_cast<float>(sun_radius);
    s.sun_illuminance_lux = static_cast<float>(sun_illuminance_lux(e.sun.distance_au));
    s.sun_disc_visible = static_cast<float>(disc_fraction_above_horizon(sun.horizontal.elevation, sun_radius));
    s.sun_airmass = static_cast<float>(airmass(sun.horizontal.elevation));
    s.equation_of_time_min = static_cast<float>(e.sun.equation_of_time_min);

    // Moon.
    const Apparent moon = apparent(e.moon_enu);
    s.moon_direction = glm::vec3(glm::normalize(world_enu * moon.enu));
    s.moon_azimuth_deg = static_cast<float>(moon.horizontal.azimuth);
    s.moon_elevation_deg = static_cast<float>(moon.horizontal.elevation);
    const double moon_radius = moon_angular_radius(e.moon_topo_distance_km);
    s.moon_angular_radius_deg = static_cast<float>(moon_radius);
    s.moon_distance_km = static_cast<float>(e.moon_topo_distance_km);
    s.moon_disc_visible =
        static_cast<float>(disc_fraction_above_horizon(moon.horizontal.elevation, moon_radius));
    s.moon_airmass = static_cast<float>(airmass(moon.horizontal.elevation));

    const MoonPhase phase = moon_phase(e.moon, e.sun);
    s.moon_illuminated_fraction = static_cast<float>(phase.illuminated_fraction);
    s.moon_phase_angle_deg = static_cast<float>(phase.phase_angle);
    s.moon_elongation_deg = static_cast<float>(phase.elongation);
    s.moon_waxing = phase.waxing;
    s.moon_bright_limb_deg = static_cast<float>(phase.bright_limb);
    s.moon_illuminance_lux =
        static_cast<float>(moon_illuminance_lux(phase.phase_angle, e.moon_topo_distance_km));

    const glm::dvec3 sun_geo = equatorial_vector(e.sun.equatorial) * (e.sun.distance_au * k_km_per_au);
    const glm::dvec3 moon_dir_eq = equatorial_vector(e.moon.equatorial);
    const glm::dvec3 moon_geo = moon_dir_eq * e.moon.distance_km;
    s.moon_sun_direction = glm::vec3(glm::normalize(world_eq * (sun_geo - moon_geo)));

    const MoonLibration lib = moon_libration(e.moon);
    s.moon_libration_longitude_deg = static_cast<float>(lib.longitude);
    s.moon_libration_latitude_deg = static_cast<float>(lib.latitude);
    s.moon_axis_position_angle_deg = static_cast<float>(lib.position_angle);
    s.moon_orientation = glm::mat3(world_eq * moon_body_axes(moon_dir_eq, lib));

    // Stars: J2000 -> mean equator of date -> horizon at mean sidereal time.
    const glm::dmat3 precession = precession_from_j2000(centuries_tt(e.days_tt));
    s.world_from_celestial =
        glm::mat3(world_enu * enu_from_equatorial(e.lst_mean, o.latitude) * precession);
    return s;
}

} // namespace engine::astro
