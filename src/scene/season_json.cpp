// Season and climate JSON codecs — see include/engine/scene/season_json.hpp.

#include "engine/scene/season_json.hpp"

#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/astro/time.hpp"
#include "engine/core/json_fields.hpp"
#include "engine/scene/season.hpp"
#include "engine/weather/climate.hpp"

namespace engine::scene {

namespace {

using Json = nlohmann::json;

constexpr core::EnumName<SeasonMode> k_modes[] = {
    {"clock", SeasonMode::clock},
    {"independent", SeasonMode::independent},
    {"manual", SeasonMode::manual},
};

[[nodiscard]] std::string range_text(float lo, float hi) {
    const auto num = [](float f) {
        std::string s = std::to_string(f);
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    };
    return "[" + num(lo) + ", " + num(hi) + "]";
}

// A curve: 1..k_climate_max_keys finite numbers within [lo, hi]; nothing
// changes on error.
void read_curve(core::JsonFields& f, const weather::ClimateCurveDesc& d, std::vector<float>& out) {
    const Json* v = f.raw(d.name);
    if (v == nullptr) return;
    const std::string expected = "expected an array of 1.." + std::to_string(weather::k_climate_max_keys) +
                                 " numbers in " + range_text(d.min, d.max) +
                                 (d.unit.empty() ? std::string{} : " (" + std::string(d.unit) + ")") +
                                 ", key i at year phase i / n";
    if (!v->is_array() || v->empty() || v->size() > weather::k_climate_max_keys) {
        f.fail(d.name, expected);
        return;
    }
    std::vector<float> keys;
    keys.reserve(v->size());
    for (const Json& k : *v) {
        if (!k.is_number()) {
            f.fail(d.name, expected);
            return;
        }
        const double x = k.get<double>();
        if (!std::isfinite(x) || x < d.min || x > d.max) {
            f.fail(d.name, expected);
            return;
        }
        keys.push_back(static_cast<float>(x));
    }
    out = std::move(keys);
}

} // namespace

Json encode_season(const SeasonState& s) {
    return {{"enabled", s.enabled},
            {"mode", std::string(season_mode_name(s.mode))},
            {"year_phase", s.year_phase},
            {"year", s.year},
            {"phase_rate", s.phase_rate},
            {"drive_clock_date", s.drive_clock_date},
            {"weather_auto", s.weather_auto},
            {"weather_seed", s.weather_seed},
            {"weather_clock_time", s.weather_clock_time}};
}

void decode_season(core::JsonFields& f, SeasonState& s) {
    f.boolean("enabled", s.enabled);
    f.enumeration("mode", s.mode, k_modes);
    // 1.0 itself is the next year's 0: accept it and wrap.
    f.number("year_phase", s.year_phase, 0.0, 1.0);
    if (s.year_phase >= 1.0) s.year_phase = 0.0;
    f.integer("year", s.year, astro::k_min_year, astro::k_max_year - 1);
    f.number("phase_rate", s.phase_rate, 0.0, k_max_phase_rate);
    f.boolean("drive_clock_date", s.drive_clock_date);
    f.boolean("weather_auto", s.weather_auto);
    f.integer("weather_seed", s.weather_seed, 0, 0xFFFFFFFFLL);
    f.boolean("weather_clock_time", s.weather_clock_time);
}

Json encode_climate(const weather::Climate& c) {
    Json out = Json::object();
    for (const weather::ClimateCurveDesc& d : weather::climate_curves()) {
        out[std::string(d.name)] = weather::curve_keys(c, d.curve);
    }
    out["temperature_variability_c"] = c.temperature_variability_c;
    out["weather_period_days"] = c.weather_period_days;
    out["transition"] = c.transition;
    return out;
}

void decode_climate(core::JsonFields& f, weather::Climate& c) {
    for (const weather::ClimateCurveDesc& d : weather::climate_curves()) {
        read_curve(f, d, weather::curve_keys(c, d.curve));
    }
    f.number("temperature_variability_c", c.temperature_variability_c, 0.0F, 15.0F);
    f.number("weather_period_days", c.weather_period_days, 0.25F, 60.0F);
    f.number("transition", c.transition, 0.0F, 1.0F);
}

} // namespace engine::scene
