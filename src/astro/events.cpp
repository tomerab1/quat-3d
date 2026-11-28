// Rising, setting, transit — see include/engine/astro/events.hpp.

#include "engine/astro/events.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "ephemeris.hpp"

namespace engine::astro {

namespace {

constexpr std::int64_t k_step_ns = 10LL * 60 * k_ns_per_second;
constexpr int          k_steps = 24 * 6; // 10-minute steps over 24 h
constexpr std::int64_t k_tolerance_ns = 50'000'000; // 50 ms
// Samples within this many degrees of the event altitude are checked for a
// grazing excursion between them (far more than the altitude can bend back
// across in two 10-minute steps).
constexpr double k_graze_deg = 1.0;

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

// Time of the maximum (or minimum) of `f` in [a, b], golden-section search
// to k_tolerance_ns (f unimodal there: one bend of the altitude curve).
template <typename F>
[[nodiscard]] UtcTime extremum(F&& f, UtcTime a, UtcTime b, bool maximum) {
    constexpr double k_inv_phi = 0.6180339887498949;
    const auto       value = [&](double t) {
        const double v = f(UtcTime{static_cast<std::int64_t>(std::llround(t))});
        return maximum ? v : -v;
    };
    double lo = static_cast<double>(a.ns);
    double hi = static_cast<double>(b.ns);
    double x1 = hi - k_inv_phi * (hi - lo);
    double x2 = lo + k_inv_phi * (hi - lo);
    double v1 = value(x1);
    double v2 = value(x2);
    while (hi - lo > static_cast<double>(k_tolerance_ns)) {
        if (v1 < v2) {
            lo = x1;
            x1 = x2;
            v1 = v2;
            x2 = lo + k_inv_phi * (hi - lo);
            v2 = value(x2);
        } else {
            hi = x2;
            x2 = x1;
            v2 = v1;
            x1 = hi - k_inv_phi * (hi - lo);
            v1 = value(x1);
        }
    }
    return UtcTime{static_cast<std::int64_t>(std::llround((lo + hi) / 2.0))};
}

struct Crossing {
    UtcTime t;
    bool    upward = false;
};

} // namespace

RiseSet rise_set(Body body, UtcTime start, double latitude, double longitude, double altitude) {
    std::array<BodySample, k_steps + 1> s{};
    std::array<double, k_steps + 1>     f{};
    for (int i = 0; i <= k_steps; ++i) {
        const auto ii = static_cast<std::size_t>(i);
        s[ii] = sample(body, UtcTime{start.ns + i * k_step_ns}, latitude, longitude, altitude);
        f[ii] = above(s[ii]);
    }
    const auto alt = [&](UtcTime t) { return above(sample(body, t, latitude, longitude, altitude)); };
    // East component: positive before the upper transit, negative after.
    const auto east = [&](UtcTime t) { return sample(body, t, latitude, longitude, altitude).enu.x; };
    const auto at = [&](int i) { return UtcTime{start.ns + i * k_step_ns}; };

    std::vector<Crossing> crossings;
    RiseSet               r;
    for (int i = 0; i < k_steps; ++i) {
        const auto ii = static_cast<std::size_t>(i);
        if ((f[ii] > 0.0) != (f[ii + 1] > 0.0)) {
            crossings.push_back({refine(alt, at(i), at(i + 1), f[ii] > 0.0), f[ii + 1] > 0.0});
        }
        // Diurnal motion is westward: at the upper meridian passage the
        // east component goes + -> −; at the lower one (below the pole)
        // it goes − -> +.
        if (!r.transit && s[ii].enu.x > 0.0 && s[ii + 1].enu.x <= 0.0) {
            r.transit = refine(east, at(i), at(i + 1), true);
        }
    }
    // Grazing excursions: a rise and set (or set and rise) closer together
    // than one step leave no sign change between samples. Where three
    // samples on one side of the event altitude bend toward it (a maximum
    // below, a minimum above) and come within k_graze_deg, find the
    // extremum (golden-section search, the vertex Montenbruck & Pfleger's
    // quadratic interpolation estimates) and test its sign.
    for (int i = 1; i < k_steps; ++i) {
        const auto   ii = static_cast<std::size_t>(i);
        const double fa = f[ii - 1];
        const double fb = f[ii];
        const double fc = f[ii + 1];
        const bool   below = fa <= 0.0 && fb <= 0.0 && fc <= 0.0;
        const bool   above_all = fa > 0.0 && fb > 0.0 && fc > 0.0;
        const bool   peak = below && fb > fa && fb >= fc; // strict on one side: a tie fires once
        const bool   dip = above_all && fb < fa && fb <= fc;
        if ((!peak && !dip) || std::abs(fb) > k_graze_deg) continue;
        const UtcTime ext = extremum(alt, at(i - 1), at(i + 1), peak);
        const double  fe = alt(ext);
        if ((fe > 0.0) == above_all) continue; // stays on its side
        crossings.push_back({refine(alt, at(i - 1), ext, above_all), peak});
        crossings.push_back({refine(alt, ext, at(i + 1), fe > 0.0), !peak});
    }
    std::sort(crossings.begin(), crossings.end(),
              [](const Crossing& x, const Crossing& y) { return x.t < y.t; });
    for (const Crossing& c : crossings) {
        if (c.upward && !r.rise) r.rise = c.t;
        if (!c.upward && !r.set) r.set = c.t;
    }
    if (crossings.empty()) {
        r.always_above = f[0] > 0.0;
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
