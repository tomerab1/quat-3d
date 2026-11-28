// WorldClock — see include/engine/scene/world_clock.hpp.

#include "engine/scene/world_clock.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include "engine/astro/events.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/sun_source.hpp"

namespace engine::scene {

namespace {

// First and last instants of the supported years.
[[nodiscard]] astro::UtcTime earliest() { return astro::utc_from_civil({astro::k_min_year, 1, 1, 0, 0, 0.0}); }
[[nodiscard]] astro::UtcTime latest() {
    return astro::utc_from_civil({astro::k_max_year, 12, 31, 23, 59, 59.0});
}

} // namespace

entt::entity find_world_clock(const entt::registry& registry) {
    for (const entt::entity e : registry.view<const WorldClock>()) return e;
    return entt::null;
}

astro::Observer observer_of(const WorldClock& c) {
    return {c.latitude_deg, c.longitude_deg, c.north_offset_deg};
}

astro::UtcTime advance_clock_time(const WorldClock& c, double seconds) {
    if (!std::isfinite(seconds) || seconds == 0.0) return c.utc;
    if (c.fixed_day) {
        const astro::UtcTime day = astro::local_midnight(c.utc, c.utc_offset_min);
        // Only the time of day matters: reduce first so long skips stay exact.
        const double       span = static_cast<double>(astro::k_ns_per_day) * 1e-9;
        const double       step = std::fmod(seconds, span);
        const std::int64_t into = astro::add_seconds(c.utc, step).ns - day.ns;
        std::int64_t       wrapped = into % astro::k_ns_per_day;
        if (wrapped < 0) wrapped += astro::k_ns_per_day;
        return astro::UtcTime{day.ns + wrapped};
    }
    const astro::UtcTime t = astro::add_seconds(c.utc, seconds);
    return std::clamp(t, earliest(), latest());
}

DayAlmanac day_almanac(const WorldClock& c) {
    DayAlmanac d;
    d.day_start = astro::local_midnight(c.utc, c.utc_offset_min);
    const double     lat = c.latitude_deg;
    const double     lon = c.longitude_deg;
    const astro::RiseSet sun = astro::rise_set(astro::Body::sun, d.day_start, lat, lon);
    d.sunrise = sun.rise;
    d.sunset = sun.set;
    d.solar_noon = sun.transit;
    d.polar_day = sun.always_above;
    d.polar_night = sun.always_below;
    const astro::RiseSet civil =
        astro::rise_set(astro::Body::sun, d.day_start, lat, lon, astro::k_civil_twilight_altitude);
    d.civil_dawn = civil.rise;
    d.civil_dusk = civil.set;
    const astro::RiseSet moon = astro::rise_set(astro::Body::moon, d.day_start, lat, lon);
    d.moonrise = moon.rise;
    d.moonset = moon.set;
    return d;
}

void world_clock_system(entt::registry& registry, float dt) {
    if (const entt::entity e = find_world_clock(registry); e != entt::null) {
        WorldClock& c = registry.get<WorldClock>(e);
        if (c.enabled && dt > 0.0F && std::isfinite(dt)) {
            c.utc = advance_clock_time(c, static_cast<double>(dt) * c.time_scale);
        }
    }
    apply_world_clock(registry);
}

void apply_world_clock(entt::registry& registry) {
    const entt::entity e = find_world_clock(registry);
    // Sky states without an enabled clock are stale (collected first: no
    // removal while iterating the pool).
    std::vector<entt::entity> stale;
    for (const entt::entity s : registry.view<astro::SkyState>()) {
        if (s != e || !registry.get<WorldClock>(s).enabled) stale.push_back(s);
    }
    for (const entt::entity s : stale) registry.remove<astro::SkyState>(s);
    if (e == entt::null) return;
    const WorldClock& c = registry.get<WorldClock>(e);
    if (!c.enabled) return;

    const astro::SkyState sky = astro::compute_sky_state(c.utc, observer_of(c));
    registry.emplace_or_replace<astro::SkyState>(e, sky);
    if (!c.drive_sun) return;
    // The scene's sun light (sun_source.hpp: the one the renderer lights with).
    if (const entt::entity l = sun_light_entity(registry); l != entt::null) {
        DirectionalLight& light = registry.get<DirectionalLight>(l);
        light.direction = -sky.sun_direction; // the light travels from the Sun
        light.intensity = c.sun_intensity * sky.sun_disc_visible;
    }
}

} // namespace engine::scene
