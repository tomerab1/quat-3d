#pragma once

// WorldClock — the scene's date, time and place (ROADMAP R22.1).
//
// One per scene (a component on one entity, like WeatherState; the codec
// refuses a second). world_clock_system() runs in Scene::tick on the
// simulation dt (the sim clock's pause / step / time scale apply), advances
// the clock by dt × time_scale, derives the sky — an astro::SkyState stored
// on the same entity, the renderer's hand-off — and, with drive_sun, writes
// the scene's DirectionalLight: its direction toward the apparent Sun and
// its intensity = sun_intensity × the fraction of the solar disc above the
// horizon (the atmosphere's transmittance LUT does the reddening and
// dimming at low sun). A disabled clock does nothing at all: the light keeps
// whatever it was given, and the SkyState is removed.

#include <cstdint>
#include <optional>

#include <entt/entity/fwd.hpp>

#include "engine/astro/sky_state.hpp"
#include "engine/astro/time.hpp"

namespace engine::scene {

struct WorldClock {
    bool enabled = true;
    // The instant (UTC). Default: the 2026 June solstice, 10:00 UTC (noon
    // local summer time at the default place).
    astro::UtcTime utc = astro::utc_from_civil({2026, 6, 21, 10, 0, 0.0}, 0);
    // Display time zone: local = UTC + this (minutes, −14 h .. +14 h).
    std::int32_t utc_offset_min = 120;
    // Observer (geodetic, degrees north / east). Default: Paris.
    double latitude_deg = 48.8566;
    double longitude_deg = 2.3522;
    // World rotation of compass north (astro/coords.hpp): 0 = north is −Z.
    double north_offset_deg = 0.0;
    // Clock seconds per simulated second: 1 = real time, 0 = frozen,
    // 3600 = an hour per second.
    double time_scale = 1.0;
    // Repeat the same local day: the time of day wraps at midnight.
    bool fixed_day = false;
    // Write the scene's DirectionalLight from the Sun.
    bool drive_sun = true;
    // DirectionalLight.intensity with the whole Sun above the horizon
    // (renderer units, as dir_light.intensity).
    float sun_intensity = 3.0F;

    friend bool operator==(const WorldClock&, const WorldClock&) = default;
};

inline constexpr double k_max_clock_time_scale = 1.0e6;

// The entity holding the scene's WorldClock, or entt::null.
[[nodiscard]] entt::entity find_world_clock(const entt::registry& registry);

[[nodiscard]] astro::Observer observer_of(const WorldClock& clock);

// `clock.utc` moved by `seconds` of clock time: wrapped into the same local
// day under fixed_day, clamped to the supported years.
[[nodiscard]] astro::UtcTime advance_clock_time(const WorldClock& clock, double seconds);

// The clock's local calendar day: sun and moon events (astro/events.hpp).
// A few hundred ephemeris evaluations — cache it per day in per-frame UI.
struct DayAlmanac {
    astro::UtcTime                day_start;   // local midnight
    std::optional<astro::UtcTime> sunrise;
    std::optional<astro::UtcTime> sunset;
    std::optional<astro::UtcTime> solar_noon;
    std::optional<astro::UtcTime> civil_dawn;  // Sun's centre at −6°
    std::optional<astro::UtcTime> civil_dusk;
    std::optional<astro::UtcTime> moonrise;
    std::optional<astro::UtcTime> moonset;
    bool                          polar_day = false;   // the Sun never sets
    bool                          polar_night = false; // the Sun never rises
};
[[nodiscard]] DayAlmanac day_almanac(const WorldClock& clock);

// Advances the enabled clock by `dt` (simulated seconds) × time_scale, then
// apply_world_clock(). Called from Scene::tick.
void world_clock_system(entt::registry& registry, float dt);

// Recomputes the clock's SkyState and drives the sun without advancing time
// (after an edit, so the next read sees the new sky). Removes stale
// SkyStates (disabled clock, clock removed).
void apply_world_clock(entt::registry& registry);

} // namespace engine::scene
