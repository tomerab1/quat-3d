// Season control surface — see include/engine/scene/season_api.hpp.

#include "engine/scene/season_api.hpp"

#include <chrono>
#include <cmath>
#include <optional>
#include <string>

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include "engine/astro/time.hpp"
#include "engine/core/json_fields.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/season.hpp"
#include "engine/scene/season_json.hpp"
#include "engine/scene/world_clock.hpp"
#include "engine/scene/world_clock_api.hpp"
#include "engine/weather/climate.hpp"
#include "engine/weather/weather_state.hpp"

namespace engine::scene {

namespace {

using Json = nlohmann::json;

constexpr std::size_t k_max_name_length = 32;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] std::string kind_list() {
    std::string s;
    for (const std::string_view n : weather::weather_kind_names()) {
        s += (s.empty() ? "\"" : ", \"") + std::string(n) + "\"";
    }
    return s;
}

// The system clock as an engine instant (for "follow the real calendar").
[[nodiscard]] astro::UtcTime now_utc() {
    const auto since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    const std::int64_t epoch_2000 = astro::days_from_civil(2000, 1, 1) * astro::k_ns_per_day;
    return astro::UtcTime{since_epoch.count() - epoch_2000};
}

[[nodiscard]] Json weights_json(const std::array<float, 4>& w) {
    Json out = Json::object();
    for (std::size_t i = 0; i < w.size(); ++i) out[std::string(k_season_names[i])] = w[i];
    return out;
}

[[nodiscard]] std::string_view dominant(const std::array<float, 4>& w) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < w.size(); ++i) {
        if (w[i] > w[best]) best = i;
    }
    return k_season_names[best];
}

// Everything a request asks for beyond the codec fields, validated before
// anything changes.
struct Extras {
    std::optional<double>              phase; // from year_phase or season
    std::optional<double>              year_minutes;
    std::optional<double>              day_minutes;
    bool                               real_calendar = false;
    bool                               climate_given = false;
    std::optional<weather::Climate>    climate; // nullopt with climate_given: back to the default
    std::optional<weather::WeatherKind> preset;
    double                             spin_up_s = 0.0;
};

} // namespace

Json season_report(const entt::registry& r) {
    const entt::entity e = find_season(r);
    const SeasonState  s = e != entt::null ? r.get<SeasonState>(e) : SeasonState{};
    Json               out = encode_season(s);
    if (e == entt::null) out["enabled"] = false;
    out["in_scene"] = e != entt::null;
    out["entity"] = e != entt::null ? Json(entt::to_integral(e)) : Json(nullptr);
    out["year_minutes"] = s.phase_rate > 0.0 ? Json(1.0 / (60.0 * s.phase_rate)) : Json(nullptr);
    const entt::entity ce = find_world_clock(r);
    const WorldClock*  clock = ce != entt::null ? &r.get<WorldClock>(ce) : nullptr;
    out["has_clock"] = clock != nullptr && clock->enabled;
    out["day_minutes"] =
        clock != nullptr && clock->time_scale > 0.0 ? Json(1440.0 / clock->time_scale) : Json(nullptr);

    const SeasonSample* smp = e != entt::null ? r.try_get<SeasonSample>(e) : nullptr;
    const std::array<float, 4> w = smp != nullptr ? smp->weights : season_weights(s.year_phase);
    out["season"] = std::string(dominant(w));
    out["weights"] = weights_json(w);
    if (smp == nullptr) return out;

    out["southern"] = smp->southern;
    out["day_length_h"] = smp->has_clock ? Json(smp->day_length_h) : Json(nullptr);
    out["polar"] = smp->polar_day ? Json("day") : (smp->polar_night ? Json("night") : Json(nullptr));
    const weather::ClimateSample& c = smp->climate;
    out["climate"] = {{"custom", smp->custom_climate},
                      {"mean_temperature_c", c.mean_temperature_c},
                      {"diurnal_range_c", c.diurnal_range_c},
                      {"temperature_c", weather::air_temperature(c, 0.0F, c.cloud_cover)},
                      {"precipitation_chance", c.precipitation_chance},
                      {"cloud_cover", c.cloud_cover},
                      {"wind_speed", c.wind_speed},
                      {"snow_fraction", weather::snow_fraction(c.mean_temperature_c)}};
    if (s.weather_auto) {
        out["weather"] = {{"kind", std::string(weather::weather_kind_name(smp->weather_day.kind))},
                          {"precipitation_type",
                           std::string(weather::precipitation_type_name(smp->weather_day.type))},
                          {"temperature_anomaly_c", smp->weather_day.temperature_anomaly_c},
                          {"period", smp->weather_period},
                          {"blend_to_next", smp->weather_blend}};
    }
    return out;
}

std::expected<void, core::Error> set_season(Scene& scene, const Json& params) {
    entt::registry&    r = scene.registry();
    const entt::entity existing = find_season(r);
    SeasonState        s = existing != entt::null ? r.get<SeasonState>(existing) : SeasonState{};
    const SeasonState  before = s;

    core::JsonFields f(params, "");
    const bool       has_phase = f.has("year_phase");
    const bool       has_auto = f.has("weather_auto");
    decode_season(f, s);
    Extras      x;
    std::string season_name;
    std::string preset_name;
    const bool  has_season = f.has("season");
    f.string("season", season_name, k_max_name_length);
    double minutes = 0.0;
    if (f.has("year_minutes")) {
        f.number("year_minutes", minutes, 0.0, k_max_year_minutes);
        x.year_minutes = minutes;
    }
    if (f.has("day_minutes")) {
        f.number("day_minutes", minutes, 1e-3, k_max_day_minutes);
        x.day_minutes = minutes;
    }
    f.boolean("real_calendar", x.real_calendar);
    const bool has_preset = f.has("weather_preset");
    f.string("weather_preset", preset_name, k_max_name_length);
    f.number("spin_up_s", x.spin_up_s, 0.0, k_max_spin_up_s);
    if (const Json* c = f.raw("climate"); c != nullptr) {
        x.climate_given = true;
        if (!c->is_null()) {
            weather::Climate climate = existing != entt::null && r.all_of<weather::Climate>(existing)
                                           ? r.get<weather::Climate>(existing)
                                           : weather::Climate{};
            core::JsonFields cf(*c, "climate");
            decode_climate(cf, climate);
            if (auto ok = cf.finish(); !ok) return std::unexpected(ok.error());
            x.climate = std::move(climate);
        }
    }
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());

    // ---- validate the extras (nothing has changed yet) ----
    if (has_season) {
        Season which{};
        if (!parse_season(season_name, which)) {
            return fail("season: expected one of \"spring\", \"summer\", \"autumn\", \"winter\"");
        }
        if (has_phase) return fail("season: give either season or year_phase, not both");
        x.phase = season_centre(which);
    } else if (has_phase) {
        x.phase = s.year_phase;
    }
    if (has_preset) {
        weather::WeatherKind kind{};
        if (!parse_weather_kind(preset_name, kind)) return fail("weather_preset: expected one of " + kind_list());
        if (has_auto && s.weather_auto) {
            return fail("weather_preset: a preset suspends weather_auto; do not also set weather_auto true");
        }
        x.preset = kind;
    }
    if (x.real_calendar) {
        if (f.has("mode") && s.mode != SeasonMode::clock) {
            return fail("real_calendar: follows the world clock, so mode must be \"clock\"");
        }
        if (x.day_minutes) return fail("day_minutes: real_calendar runs the clock in real time");
        if (x.phase) return fail("real_calendar: the phase follows today's date; do not also set it");
        s.mode = SeasonMode::clock;
    }
    const entt::entity ce = find_world_clock(r);
    if (x.day_minutes && ce == entt::null) {
        return fail("day_minutes: sets the world clock's speed, and the scene has no world clock "
                    "(create one with time.set first)");
    }
    if (x.phase && s.mode == SeasonMode::clock && !x.real_calendar &&
        (ce == entt::null || !r.get<WorldClock>(ce).enabled)) {
        return fail("year_phase / season: in clock mode the phase follows the world clock, and the "
                    "scene has no enabled one; use mode \"manual\" or \"independent\", or add a clock "
                    "with time.set");
    }

    // ---- apply ----
    if (x.real_calendar) {
        const Json clock = {{"datetime", astro::format_iso8601(now_utc(), 0)},
                            {"time_scale", 1.0},
                            {"fixed_day", false},
                            {"enabled", true}};
        if (auto ok = set_world_clock(scene, clock); !ok) return std::unexpected(ok.error());
    }
    if (x.year_minutes) s.phase_rate = *x.year_minutes > 0.0 ? 1.0 / (60.0 * *x.year_minutes) : 0.0;
    if (x.day_minutes) {
        WorldClock& c = r.get<WorldClock>(find_world_clock(r));
        c.time_scale = std::min(1440.0 / *x.day_minutes, k_max_clock_time_scale);
    }
    if (x.phase) {
        if (s.mode == SeasonMode::clock) {
            // The clock's date moves; the phase follows it on apply.
            WorldClock& c = r.get<WorldClock>(find_world_clock(r));
            const int   year = season_year_at(c.utc, c.latitude_deg < 0.0);
            c.utc = clock_date_for_phase(c, year, *x.phase);
            s.year_phase = before.year_phase;
        } else {
            s.year_phase = *x.phase;
        }
    }
    if (x.preset) s.weather_auto = false;

    entt::entity e = existing;
    if (e == entt::null) e = scene.create_entity("season");
    r.emplace_or_replace<SeasonState>(e, s);
    if (x.climate_given) {
        if (x.climate) {
            r.emplace_or_replace<weather::Climate>(e, *x.climate);
        } else {
            r.remove<weather::Climate>(e);
        }
    }
    apply_world_clock(r);
    if (x.spin_up_s > 0.0) {
        spin_up_season(r, x.spin_up_s);
    } else {
        apply_season(r);
    }
    if (x.preset) apply_weather_preset(r, *x.preset);
    return {};
}

} // namespace engine::scene
