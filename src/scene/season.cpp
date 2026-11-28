// SeasonState — see include/engine/scene/season.hpp.

#include "engine/scene/season.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/astro/events.hpp"
#include "engine/astro/sky_state.hpp"
#include "engine/astro/sun.hpp"
#include "engine/scene/world_clock.hpp"
#include "engine/weather/weather_state.hpp"

namespace engine::scene {

namespace {

constexpr std::array<std::string_view, 3> k_mode_names = {"clock", "independent", "manual"};
constexpr double k_seconds_per_day = 86400.0;
constexpr double k_min_spin_step = 0.05;

[[nodiscard]] double wrap01(double x) {
    if (!std::isfinite(x)) return 0.0;
    const double f = x - std::floor(x);
    return f >= 1.0 ? 0.0 : f;
}

// (x wrapped into [-0.5, 0.5)): the shortest signed phase difference.
[[nodiscard]] double wrap_half(double x) { return wrap01(x + 0.5) - 0.5; }

[[nodiscard]] std::int32_t clamp_year(std::int64_t y) {
    // The last season year still ends inside the supported range.
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(y, astro::k_min_year, astro::k_max_year - 1));
}

// Moves the phase by `delta` years, carrying whole years into `year`.
void advance_phase(SeasonState& s, double delta) {
    if (!std::isfinite(delta) || delta == 0.0) return;
    const double p = wrap01(s.year_phase) + delta;
    const double whole = std::floor(p);
    s.year = clamp_year(static_cast<std::int64_t>(s.year) + static_cast<std::int64_t>(whole));
    s.year_phase = wrap01(p - whole);
}

// Local mean solar time (hours) at the clock, corrected by the equation of
// time when the clock's sky is known (apparent solar time).
[[nodiscard]] double solar_hour(const WorldClock& c, const astro::SkyState* sky) {
    std::int64_t into = c.utc.ns % astro::k_ns_per_day;
    if (into < 0) into += astro::k_ns_per_day;
    double h = static_cast<double>(into) * 1e-9 / 3600.0 + c.longitude_deg / 15.0;
    if (sky != nullptr) h += static_cast<double>(sky->equation_of_time_min) / 60.0;
    return 24.0 * wrap01(h / 24.0);
}

void derive_sample(entt::registry& r, entt::entity e, SeasonState& s, const WorldClock* clock,
                   entt::entity clock_entity) {
    SeasonSample& smp = r.get_or_emplace<SeasonSample>(e);
    smp.year_phase = s.year_phase;
    smp.weights = season_weights(s.year_phase);
    smp.has_clock = clock != nullptr;
    smp.southern = clock != nullptr && clock->latitude_deg < 0.0;
    std::optional<double> hour;
    if (clock != nullptr) {
        const astro::UtcTime midnight = astro::local_midnight(clock->utc, clock->utc_offset_min);
        if (midnight != smp.almanac_day) {
            // A few hundred ephemeris evaluations: once per local day.
            const DayAlmanac day = day_almanac(*clock);
            smp.almanac_day = midnight;
            smp.polar_day = day.polar_day;
            smp.polar_night = day.polar_night;
            if (day.sunrise && day.sunset) {
                double len = static_cast<double>(day.sunset->ns - day.sunrise->ns) * 1e-9 / 3600.0;
                if (len < 0.0) len += 24.0; // the Sun set before it rose this local day
                smp.day_length_h = static_cast<float>(std::clamp(len, 0.0, 24.0));
            } else {
                smp.day_length_h = day.polar_day ? 24.0F : (day.polar_night ? 0.0F : 12.0F);
            }
        }
        hour = solar_hour(*clock, r.try_get<astro::SkyState>(clock_entity));
    } else {
        smp.almanac_day = astro::UtcTime{-1};
        smp.day_length_h = 12.0F;
        smp.polar_day = smp.polar_night = false;
    }

    const weather::Climate* custom = r.try_get<weather::Climate>(e);
    smp.custom_climate = custom != nullptr;
    const weather::Climate climate = weather::sanitized(custom != nullptr ? *custom : weather::Climate{});
    smp.climate = weather::sample_climate(climate, s.year_phase, hour);
    smp.season_days = season_days(s.year, s.year_phase);
    if (!s.weather_auto) return;

    entt::entity we = weather::find_weather(r);
    if (we == entt::null) {
        r.emplace<weather::WeatherState>(e);
        we = e;
    }
    weather::WeatherState&    w = r.get<weather::WeatherState>(we);
    const weather::AutoWeather aw =
        weather::auto_weather(climate, s.weather_seed, smp.season_days, smp.climate, weather::sanitized(w));
    w = aw.state;
    smp.weather_day = aw.day;
    smp.weather_period = aw.period;
    smp.weather_blend = aw.blend;
}

} // namespace

double season_centre(Season season) { return 0.125 + 0.25 * static_cast<double>(season); }

bool parse_season(std::string_view name, Season& out) {
    for (std::size_t i = 0; i < k_season_names.size(); ++i) {
        if (k_season_names[i] == name) {
            out = static_cast<Season>(i);
            return true;
        }
    }
    return false;
}

std::string_view season_mode_name(SeasonMode mode) {
    const auto i = static_cast<std::size_t>(mode);
    return i < k_mode_names.size() ? k_mode_names[i] : "clock";
}

bool parse_season_mode(std::string_view name, SeasonMode& out) {
    for (std::size_t i = 0; i < k_mode_names.size(); ++i) {
        if (k_mode_names[i] == name) {
            out = static_cast<SeasonMode>(i);
            return true;
        }
    }
    return false;
}

double year_phase_at(astro::UtcTime utc, double latitude_deg) {
    const astro::SunPosition sun = astro::sun_position(astro::days_tt_since_j2000(utc));
    return wrap01(sun.longitude / 360.0 + (latitude_deg < 0.0 ? 0.5 : 0.0));
}

astro::UtcTime instant_of_phase(std::int32_t year, double phase, bool southern) {
    const std::int32_t y = clamp_year(year);
    const double       p = wrap01(phase);
    // First guess: the equinox's usual date, then a uniform year.
    const astro::UtcTime start = southern ? astro::utc_from_civil({y, 9, 22, 12, 0, 0.0})
                                          : astro::utc_from_civil({y, 3, 20, 12, 0, 0.0});
    const double  lat = southern ? -1.0 : 1.0;
    const double  year_s = weather::k_tropical_year_days * k_seconds_per_day;
    astro::UtcTime t = astro::add_seconds(start, p * year_s);
    // Newton steps with the mean slope (1 / tropical year): the true rate
    // differs by the orbit's eccentricity (~3 %), so each step gains about
    // 1.5 decimal digits; six reach well under a second.
    for (int i = 0; i < 6; ++i) {
        const double err = wrap_half(p - year_phase_at(t, lat));
        if (std::abs(err) * year_s < 1e-3) break;
        t = astro::add_seconds(t, err * year_s);
    }
    return t;
}

std::int32_t season_year_at(astro::UtcTime utc, bool southern) {
    const int          y = astro::civil_from_utc(utc).year;
    const std::int32_t year = clamp_year(y);
    if (utc < instant_of_phase(year, 0.0, southern)) return clamp_year(static_cast<std::int64_t>(year) - 1);
    return year;
}

double season_days(std::int32_t year, double phase) {
    return (static_cast<double>(year - 2000) + wrap01(phase)) * weather::k_tropical_year_days;
}

std::array<float, 4> season_weights(double phase) {
    std::array<float, 4> w{};
    const double p = wrap01(phase);
    for (std::size_t i = 0; i < w.size(); ++i) {
        const double d = std::abs(wrap_half(p - season_centre(static_cast<Season>(i))));
        if (d < 0.25) {
            // Hann window one season wide: cos^2 of neighbours sum to 1.
            const double c = std::cos(2.0 * std::numbers::pi * d);
            w[i] = static_cast<float>(c * c);
        }
    }
    return w;
}

astro::UtcTime clock_date_for_phase(const WorldClock& clock, std::int32_t year, double phase) {
    const bool           south = clock.latitude_deg < 0.0;
    const astro::UtcTime target = instant_of_phase(year, phase, south);
    const astro::UtcTime day = astro::local_midnight(target, clock.utc_offset_min);
    const std::int64_t   into = clock.utc.ns - astro::local_midnight(clock.utc, clock.utc_offset_min).ns;
    const astro::UtcTime t{day.ns + into};
    return astro::in_supported_range(t) ? t : clock.utc;
}

entt::entity find_season(const entt::registry& registry) {
    for (const entt::entity e : registry.view<const SeasonState>()) return e;
    return entt::null;
}

void season_system(entt::registry& r, float dt) {
    const entt::entity e = find_season(r);
    // Samples without an enabled season are stale (collected first: no
    // removal while iterating the pool).
    std::vector<entt::entity> stale;
    for (const entt::entity s : r.view<SeasonSample>()) {
        const SeasonState* owner = r.try_get<SeasonState>(s);
        if (s != e || owner == nullptr || !owner->enabled) stale.push_back(s);
    }
    for (const entt::entity s : stale) r.remove<SeasonSample>(s);
    if (e == entt::null) return;
    SeasonState& s = r.get<SeasonState>(e);
    if (!s.enabled) return;

    const entt::entity ce = find_world_clock(r);
    WorldClock*        clock = ce != entt::null ? &r.get<WorldClock>(ce) : nullptr;
    if (clock != nullptr && !clock->enabled) clock = nullptr;

    s.year_phase = wrap01(s.year_phase);
    s.year = clamp_year(s.year);
    switch (s.mode) {
    case SeasonMode::clock:
        if (clock != nullptr) {
            s.year_phase = year_phase_at(clock->utc, clock->latitude_deg);
            s.year = season_year_at(clock->utc, clock->latitude_deg < 0.0);
        }
        break;
    case SeasonMode::independent:
        if (dt > 0.0F && std::isfinite(dt) && s.phase_rate > 0.0) {
            advance_phase(s, static_cast<double>(dt) * std::min(s.phase_rate, k_max_phase_rate));
        }
        break;
    case SeasonMode::manual: break;
    }

    if (clock != nullptr && s.drive_clock_date && s.mode != SeasonMode::clock) {
        const astro::UtcTime t = clock_date_for_phase(*clock, s.year, s.year_phase);
        if (t != clock->utc) {
            clock->utc = t;
            apply_world_clock(r); // the sky of the new date this frame
        }
    }
    derive_sample(r, e, s, clock, ce);
}

void apply_season(entt::registry& registry) { season_system(registry, 0.0F); }

float weather_dt(const entt::registry& r, float dt) {
    const entt::entity e = find_season(r);
    if (e == entt::null) return dt;
    const SeasonState& s = r.get<SeasonState>(e);
    if (!s.enabled || !s.weather_clock_time) return dt;
    const entt::entity ce = find_world_clock(r);
    if (ce == entt::null) return dt;
    const WorldClock& c = r.get<WorldClock>(ce);
    if (!c.enabled) return dt;
    // step_weather integrates exactly (exponential approach): any step is stable.
    return static_cast<float>(static_cast<double>(dt) * c.time_scale);
}

void apply_weather_preset(entt::registry& r, weather::WeatherKind kind) {
    const entt::entity e = find_season(r);
    if (e != entt::null) r.get<SeasonState>(e).weather_auto = false;
    // The sample is missing while the season is disabled: the climate at
    // its phase, daily mean.
    const SeasonSample* smp = e != entt::null ? r.try_get<SeasonSample>(e) : nullptr;
    weather::ClimateSample now;
    if (smp != nullptr) {
        now = smp->climate;
    } else {
        const weather::Climate* custom = e != entt::null ? r.try_get<weather::Climate>(e) : nullptr;
        const double            phase = e != entt::null ? r.get<SeasonState>(e).year_phase : 0.0;
        now = weather::sample_climate(weather::sanitized(custom != nullptr ? *custom : weather::Climate{}),
                                      phase, std::nullopt);
    }
    entt::entity we = weather::find_weather(r);
    if (we == entt::null) {
        we = e != entt::null ? e : r.create();
        r.emplace<weather::WeatherState>(we);
    }
    weather::WeatherState& w = r.get<weather::WeatherState>(we);
    w = weather::weather_preset(kind, now, weather::sanitized(w));
}

void spin_up_season(entt::registry& r, double seconds, float step) {
    const entt::entity e = find_season(r);
    if (e == entt::null || !r.get<SeasonState>(e).enabled) return;
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        apply_season(r);
        return;
    }
    seconds = std::min(seconds, k_max_spin_up_s);
    const double dt_step = std::max(static_cast<double>(step), k_min_spin_step);

    const SeasonState  saved = r.get<SeasonState>(e);
    const entt::entity ce = find_world_clock(r);
    const bool         has_clock = ce != entt::null && r.get<WorldClock>(ce).enabled;
    const astro::UtcTime saved_utc = has_clock ? r.get<WorldClock>(ce).utc : astro::UtcTime{};

    // Rewind: the clock's time (its own speed), and outside clock mode the
    // phase (manual seasons spin up as if running at phase_rate).
    if (has_clock) {
        WorldClock& c = r.get<WorldClock>(ce);
        c.utc = advance_clock_time(c, -seconds * c.time_scale);
    }
    if (saved.mode != SeasonMode::clock) {
        SeasonState& s = r.get<SeasonState>(e);
        s.mode = SeasonMode::independent;
        advance_phase(s, -seconds * std::clamp(saved.phase_rate, 0.0, k_max_phase_rate));
    }
    apply_world_clock(r);
    apply_season(r);

    double left = seconds;
    while (left > 1e-9) {
        const auto dt = static_cast<float>(std::min(dt_step, left));
        left -= static_cast<double>(dt);
        weather::weather_system(r, weather_dt(r, dt));
        world_clock_system(r, dt);
        season_system(r, dt);
    }

    // Land exactly on the state the spin-up led to.
    SeasonState& s = r.get<SeasonState>(e);
    s.mode = saved.mode;
    s.year_phase = saved.year_phase;
    s.year = saved.year;
    if (has_clock) r.get<WorldClock>(ce).utc = saved_utc;
    apply_world_clock(r);
    apply_season(r);
}

} // namespace engine::scene
