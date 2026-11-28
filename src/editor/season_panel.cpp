#include "season_panel.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include "engine/scene/season.hpp"
#include "engine/scene/world_clock.hpp"
#include "engine/weather/climate.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

constexpr std::array k_modes = {
    ui::EnumItem{static_cast<int>(scene::SeasonMode::clock), "Clock",
                 "The phase follows the world clock's Sun (real dates, both hemispheres)"},
    ui::EnumItem{static_cast<int>(scene::SeasonMode::independent), "Independent",
                 "The phase advances at its own rate: a short year decoupled from the day"},
    ui::EnumItem{static_cast<int>(scene::SeasonMode::manual), "Manual", "Hold this season"},
};

constexpr std::array k_kinds = {
    ui::EnumItem{0, "Clear", nullptr},   ui::EnumItem{1, "Fair", nullptr},
    ui::EnumItem{2, "Overcast", nullptr}, ui::EnumItem{3, "Showers", nullptr},
    ui::EnumItem{4, "Rain", nullptr},    ui::EnumItem{5, "Storm", nullptr},
    ui::EnumItem{6, "Snow", nullptr},
};
static_assert(k_kinds.size() == weather::k_weather_kind_count);

void draw_phase(entt::registry& r, scene::SeasonState& s) {
    float phase = static_cast<float>(s.year_phase);
    const entt::entity ce = scene::find_world_clock(r);
    if (s.mode == scene::SeasonMode::clock) {
        // Scrubbing moves the clock's date (its time of day kept).
        if (ce == entt::null) {
            ui::prop_note("No world clock: the phase holds until one is added.");
        }
        const bool moved = ui::prop_float("Year phase", phase,
                                          {.speed = 0.002F, .min = 0.0F, .max = 0.9999F, .slider = true, .clamp = true},
                                          {.tooltip = "0 spring equinox, 0.25 summer solstice, 0.5 autumn "
                                                      "equinox, 0.75 winter solstice (moves the clock's date)",
                                           .read_only = ce == entt::null});
        if (moved && ce != entt::null) {
            scene::WorldClock& c = r.get<scene::WorldClock>(ce);
            c.utc = scene::clock_date_for_phase(c, s.year, phase);
            scene::apply_world_clock(r);
        }
        return;
    }
    if (ui::prop_float("Year phase", phase,
                       {.speed = 0.002F, .min = 0.0F, .max = 0.9999F, .slider = true, .clamp = true},
                       {.tooltip = "0 spring equinox, 0.25 summer solstice, 0.5 autumn equinox, 0.75 "
                                   "winter solstice"})) {
        s.year_phase = phase;
    }
    int year = s.year;
    if (ui::prop_int("Season year", year, {.min = 1800, .max = 2199},
                     {.tooltip = "Varies the weather from year to year; anchors the driven clock date"})) {
        s.year = year;
    }
    // The rate as minutes per year (0 = stopped).
    float minutes = s.phase_rate > 0.0 ? static_cast<float>(1.0 / (60.0 * s.phase_rate)) : 0.0F;
    if (ui::prop_float("Year length", minutes, {.speed = 0.5F, .min = 0.0F, .max = 100000.0F, .format = "%.1f min"},
                       {.tooltip = "Real minutes per year at simulation speed 1 (0 stops the year)"})) {
        s.phase_rate = minutes > 0.0F ? 1.0 / (60.0 * static_cast<double>(minutes)) : 0.0;
    }
    (void)ui::prop_bool("Drive clock date", s.drive_clock_date,
                        {.tooltip = "Write the world clock's date from the phase: sun path, day length "
                                    "and dusk follow the season"});
}

void draw_readouts(const entt::registry& r, entt::entity e, const scene::SeasonState& s) {
    const auto* smp = r.try_get<scene::SeasonSample>(e);
    if (smp == nullptr) return;
    ui::prop_separator("Now");
    std::array<char, 128> buf{};
    std::snprintf(buf.data(), buf.size(), "spring %.2f  summer %.2f  autumn %.2f  winter %.2f",
                  static_cast<double>(smp->weights[0]), static_cast<double>(smp->weights[1]),
                  static_cast<double>(smp->weights[2]), static_cast<double>(smp->weights[3]));
    ui::prop_value("Weights", buf.data(), "Season weights (overlapping Hann windows; they sum to 1)", true);
    if (smp->has_clock) {
        std::snprintf(buf.data(), buf.size(), "%.2f h%s", static_cast<double>(smp->day_length_h),
                      smp->polar_day ? " (polar day)" : (smp->polar_night ? " (polar night)" : ""));
        ui::prop_value("Day length", buf.data(), "Sunrise to sunset on the clock's date", true);
    }
    const weather::ClimateSample& c = smp->climate;
    std::snprintf(buf.data(), buf.size(), "%.1f C (mean %.1f, range %.1f)",
                  static_cast<double>(weather::air_temperature(c, 0.0F, c.cloud_cover)),
                  static_cast<double>(c.mean_temperature_c), static_cast<double>(c.diurnal_range_c));
    ui::prop_value("Temperature", buf.data(), "The climate now (without the period's anomaly)", true);
    std::snprintf(buf.data(), buf.size(), "%.0f %%, clouds %.0f %%, wind %.1f m/s",
                  100.0 * static_cast<double>(c.precipitation_chance), 100.0 * static_cast<double>(c.cloud_cover),
                  static_cast<double>(c.wind_speed));
    ui::prop_value("Precipitation", buf.data(), "Chance per weather period, mean cloud cover and wind", true);
    if (s.weather_auto) {
        std::snprintf(buf.data(), buf.size(), "%s, %+.1f C (period %lld)",
                      std::string(weather::weather_kind_name(smp->weather_day.kind)).c_str(),
                      static_cast<double>(smp->weather_day.temperature_anomaly_c),
                      static_cast<long long>(smp->weather_period));
        ui::prop_value("Weather", buf.data(), "The weather sequence's current period", true);
    }
}

void plot_curve(const char* label, const std::vector<float>& keys, const char* unit) {
    std::array<float, 96> values{};
    float lo = 1e30F;
    float hi = -1e30F;
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = weather::sample_curve(keys, static_cast<double>(i) / static_cast<double>(values.size()));
        lo = std::min(lo, values[i]);
        hi = std::max(hi, values[i]);
    }
    ui::prop_begin(label, "Over the year: spring equinox at the left, winter at the right", false, false);
    std::array<char, 64> overlay{};
    std::snprintf(overlay.data(), overlay.size(), "%.1f .. %.1f %s", static_cast<double>(lo),
                  static_cast<double>(hi), unit);
    ImGui::PlotLines("##curve", values.data(), static_cast<int>(values.size()), 0, overlay.data(), lo,
                     hi > lo ? hi : lo + 1.0F, ImVec2(0.0F, ImGui::GetFrameHeight() * 2.0F));
    (void)ui::prop_end(false, false);
}

} // namespace

void draw_season(entt::registry& r, entt::entity e, scene::SeasonState& s) {
    // `s` is the entity's component: edits land directly.
    const scene::SeasonState before = s;
    (void)ui::prop_bool("Enabled", s.enabled, {.tooltip = "Derive the season and drive the weather"});
    (void)ui::prop_enum("Mode", s.mode, k_modes, {.tooltip = "Where the year phase comes from"});
    draw_phase(r, s);
    ui::prop_separator("Weather");
    (void)ui::prop_bool("Auto weather", s.weather_auto,
                        {.tooltip = "Drive the scene's weather from the climate's seeded sequence "
                                    "(a hand-set weather suspends it)"});
    (void)ui::prop_u32("Seed", s.weather_seed, {}, {.tooltip = "Seed of the weather sequence"});
    (void)ui::prop_bool("Clock-time surfaces", s.weather_clock_time,
                        {.tooltip = "Wetness and snow evolve at the world clock's speed, so a fast day "
                                    "dries and thaws at the pace of its sun"});
    // Edits show at once, also while the simulation is paused.
    if (before != s) scene::apply_season(r);
    int preset = -1;
    if (ui::prop_enum("Apply preset", preset, k_kinds,
                      {.tooltip = "A weather in this season's climate: showers fall as snow when it is "
                                  "cold (suspends Auto weather)"}) &&
        preset >= 0) {
        // A plain function over the registry (tested through season.set).
        scene::apply_weather_preset(r, static_cast<weather::WeatherKind>(preset));
    }
    draw_readouts(r, e, s);
}

void draw_climate(entt::registry& r, entt::entity /*e*/, weather::Climate& c) {
    const weather::Climate before = c;
    plot_curve("Temperature", c.temperature_c, "C");
    plot_curve("Diurnal range", c.diurnal_range_c, "C");
    plot_curve("Precipitation", c.precipitation_chance, "");
    plot_curve("Cloud cover", c.cloud_cover, "");
    plot_curve("Wind", c.wind_speed, "m/s");
    ui::prop_note("Curves are edited as data (scene files, season.set {climate}, MCP).");
    (void)ui::prop_float("Variability", c.temperature_variability_c,
                         {.speed = 0.05F, .min = 0.0F, .max = 15.0F, .format = "%.2f C", .clamp = true},
                         {.tooltip = "Standard deviation of a weather period's temperature anomaly"});
    (void)ui::prop_float("Period", c.weather_period_days,
                         {.speed = 0.05F, .min = 0.25F, .max = 60.0F, .format = "%.2f days", .clamp = true},
                         {.tooltip = "Length of one weather period, in days of the season year"});
    (void)ui::prop_float("Transition", c.transition,
                         {.speed = 0.01F, .min = 0.0F, .max = 1.0F, .slider = true, .clamp = true},
                         {.tooltip = "Fraction of each period spent easing into the next"});
    if (before != c) scene::apply_season(r);
}

} // namespace engine::editor
