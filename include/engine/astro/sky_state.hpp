#pragma once

// SkyState — everything the renderer needs about the sky at one instant,
// for one observer (ROADMAP R22.1; consumed by R22.2's night-sky renderer).
// Plain data in float precision (the ephemerides run in double), every
// direction a unit vector in WORLD space (see coords.hpp for the axis
// convention and `north_offset_deg`).
//
// Illuminances are extra-atmospheric at normal incidence: the renderer
// applies the atmosphere (its transmittance LUT) itself. `*_airmass` and
// coords.hpp's extinction_factor() give a quick estimate where no LUT is
// available.

#include <glm/glm.hpp>

#include "engine/astro/time.hpp"

namespace engine::astro {

struct Observer {
    double latitude = 0.0;         // geodetic, degrees north
    double longitude = 0.0;        // degrees east
    double north_offset_deg = 0.0; // world rotation of compass north (coords.hpp)
};

struct SkyState {
    double julian_date_ut = 0.0;
    double local_sidereal_time_deg = 0.0; // mean, [0, 360)

    // ---- Sun ----------------------------------------------------------------
    glm::vec3 sun_direction{0.0F, 1.0F, 0.0F}; // toward the apparent (refracted) Sun
    float sun_azimuth_deg = 0.0F;              // compass bearing
    float sun_elevation_deg = 90.0F;           // apparent (refracted), topocentric
    float sun_angular_radius_deg = 0.0F;
    float sun_illuminance_lux = 0.0F; // extra-atmospheric, normal incidence
    float sun_disc_visible = 1.0F;    // fraction of the disc above the horizon
    float sun_airmass = 1.0F;         // Kasten & Young; the horizon value below it
    float equation_of_time_min = 0.0F;

    // ---- Moon ---------------------------------------------------------------
    glm::vec3 moon_direction{0.0F, -1.0F, 0.0F}; // toward the apparent (topocentric, refracted) Moon
    float moon_azimuth_deg = 0.0F;
    float moon_elevation_deg = -90.0F;
    float moon_angular_radius_deg = 0.0F; // topocentric
    float moon_distance_km = 0.0F;        // topocentric
    float moon_illuminance_lux = 0.0F;    // extra-atmospheric (Krisciunas & Schaefer 1991)
    float moon_disc_visible = 0.0F;
    float moon_airmass = 1.0F;
    float moon_illuminated_fraction = 0.0F;
    float moon_phase_angle_deg = 180.0F; // 0 = full, 180 = new
    float moon_elongation_deg = 0.0F;    // geocentric Sun–Moon angle
    bool  moon_waxing = true;
    float moon_bright_limb_deg = 0.0F;   // position angle χ from celestial north through east
    // Direction of the sunlight arriving at the Moon (Moon -> Sun), world:
    // light the Moon's sphere with it to get the phase and terminator.
    glm::vec3 moon_sun_direction{0.0F, 1.0F, 0.0F};
    // Optical libration and the axis (Meeus ch. 53).
    float moon_libration_longitude_deg = 0.0F;
    float moon_libration_latitude_deg = 0.0F;
    float moon_axis_position_angle_deg = 0.0F;
    // Selenographic -> world rotation: columns are the world directions of
    // the Moon's body axes (x: 0°N 0°E, the mean sub-Earth point; y: 0°N 90°E;
    // z: the north pole). Apply to the Moon's texture-space normal.
    glm::mat3 moon_orientation{1.0F};

    // ---- Stars --------------------------------------------------------------
    // J2000 equatorial (ICRS) unit vectors -> world: precession to date,
    // Earth's rotation (mean sidereal time) and the observer's latitude.
    // Star catalogue positions go through this; the celestial north pole is
    // world_from_celestial[2].
    glm::mat3 world_from_celestial{1.0F};
};

// The sky over `observer` at `t` (a few hundred trigonometric terms; cheap
// enough to evaluate every tick).
[[nodiscard]] SkyState compute_sky_state(UtcTime t, const Observer& observer);

// Fraction of a disc of angular radius `radius` whose centre is at
// `elevation` that lies above the horizon (both degrees).
[[nodiscard]] double disc_fraction_above_horizon(double elevation, double radius);

} // namespace engine::astro
