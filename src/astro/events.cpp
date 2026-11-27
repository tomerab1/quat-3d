// Rising, setting, transit — see include/engine/astro/events.hpp.

#include "engine/astro/events.hpp"

#include <array>
#include <cmath>

#include "ephemeris.hpp"

namespace engine::astro {

namespace {

constexpr std::int64_t k_step_ns = 10LL * 60 * k_ns_per_second;
constexpr int          k_steps = 24 * 6; // 10-minute steps over 24 h
constexpr std::int64_t k_tolerance_ns = 50'000'000; // 50 ms

// Topocentric direction of `body` (ENU) and the altitude its event happens
// at, for one instant.
struct BodySample {
    glm::dvec3 enu{0.0, 0.0, 1.0};
    double     event_altitude = 0.0;
};

[[nodiscard]] BodySample sample(Body body, UtcTime t, double latitude, double longitude,
                                double sun_altitude) {
    const detail::Ephemeris e = detail::ephemeris(t, latitude, longitude, body == Body::moon);
    if (body == Body::sun) return {e.sun_enu, sun_altitude};
    // Upper limb with 34' of refraction (topocentric semidiameter).
    return {e.moon_enu, -34.0 / 60.0 - moon_angular_radius(e.moon_topo_distance_km)};
}

// Altitude above the event altitude, degrees.
[[nodiscard]] double above(const BodySample& s) {
    return std::asin(glm::clamp(s.enu.z, -1.0, 1.0)) / k_deg - s.event_altitude;
}

// Bisects [a, b] (f(a) and f(b) of opposite sign) to k_tolerance_ns and
// returns the midpoint.
template <typename F>
[[nodiscard]] UtcTime refine(F&& f, UtcTime a, UtcTime b, bool a_positive) {
    while (b.ns - a.ns > k_tolerance_ns) {
        const UtcTime mid{a.ns + (b.ns - a.ns) / 2};
        if ((f(mid) > 0.0) == a_positive) {
            a = mid;
        } else {
            b = mid;
        }
    }
    return UtcTime{a.ns + (b.ns - a.ns) / 2};
}

} // namespace

RiseSet rise_set(Body body, UtcTime start, double latitude, double longitude, double altitude) {
    std::array<BodySample, k_steps + 1> s{};
    for (int i = 0; i <= k_steps; ++i) {
        s[static_cast<std::size_t>(i)] =
            sample(body, UtcTime{start.ns + i * k_step_ns}, latitude, longitude, altitude);
    }
    const auto alt = [&](UtcTime t) { return above(sample(body, t, latitude, longitude, altitude)); };
    // East component: positive before the upper transit, negative after.
    const auto east = [&](UtcTime t) { return sample(body, t, latitude, longitude, altitude).enu.x; };

    RiseSet r;
    bool    any_crossing = false;
    for (int i = 0; i < k_steps; ++i) {
        const auto      ii = static_cast<std::size_t>(i);
        const UtcTime   a{start.ns + i * k_step_ns};
        const UtcTime   b{a.ns + k_step_ns};
        const double    fa = above(s[ii]);
        const double    fb = above(s[ii + 1]);
        if ((fa > 0.0) != (fb > 0.0)) {
            any_crossing = true;
            const UtcTime t = refine(alt, a, b, fa > 0.0);
            if (fb > 0.0 && !r.rise) r.rise = t;
            if (fb <= 0.0 && !r.set) r.set = t;
        }
        // Diurnal motion is westward: at the upper meridian passage the
        // east component goes + -> −; at the lower one (below the pole)
        // it goes − -> +.
        if (!r.transit && s[ii].enu.x > 0.0 && s[ii + 1].enu.x <= 0.0) {
            r.transit = refine(east, a, b, true);
        }
    }
    if (!any_crossing) {
        r.always_above = above(s[0]) > 0.0;
        r.always_below = !r.always_above;
    }
    return r;
}

UtcTime local_midnight(UtcTime t, int utc_offset_min) {
    const std::int64_t offset_ns = static_cast<std::int64_t>(utc_offset_min) * 60 * k_ns_per_second;
    const std::int64_t local = t.ns + offset_ns;
    std::int64_t       days = local / k_ns_per_day;
    if (local % k_ns_per_day != 0 && local < 0) --days;
    return UtcTime{days * k_ns_per_day - offset_ns};
}

} // namespace engine::astro
