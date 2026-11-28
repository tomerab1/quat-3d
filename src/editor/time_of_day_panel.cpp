#include "time_of_day_panel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

#include <entt/entity/registry.hpp>
#include <imgui.h>

#include "engine/astro/events.hpp"
#include "engine/astro/moon.hpp"
#include "engine/astro/sky_state.hpp"
#include "engine/astro/time.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/world_clock.hpp"
#include "ui/icons.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

struct Place {
    const char* name;
    double      latitude;
    double      longitude;
    int         utc_offset_min; // standard time
};
// Presets spanning latitudes (polar day / night, tropics, both hemispheres).
constexpr std::array k_places = {
    Place{"Paris", 48.8566, 2.3522, 60},          Place{"London", 51.5072, -0.1276, 0},
    Place{"New York", 40.7128, -74.0060, -300},   Place{"Tokyo", 35.6762, 139.6503, 540},
    Place{"Sydney", -33.8688, 151.2093, 600},     Place{"Reykjavik", 64.1466, -21.9426, 0},
    Place{"Tromsø", 69.6492, 18.9553, 60},        Place{"Quito (equator)", -0.1807, -78.4678, -300},
    Place{"Cape Town", -33.9249, 18.4241, 120},   Place{"Svalbard", 78.2232, 15.6267, 60},
};

// Speed presets (1x is the time scale's reset arrow): a minute, ten minutes
// and an hour of clock time per second.
constexpr std::array<double, 3> k_speeds = {60.0, 600.0, 3600.0};
constexpr std::array k_speed_labels = {"60x", "600x", "3600x"};
constexpr std::array k_speed_tips = {"A minute per second", "Ten minutes per second",
                                     "An hour per second"};

// The day's events change only with the day or the place: computed once.
struct AlmanacCache {
    astro::UtcTime    day_start{-1};
    double            latitude = 1e9;
    double            longitude = 1e9;
    scene::DayAlmanac almanac;
};
AlmanacCache g_almanac; // editor-only display cache (main thread)

[[nodiscard]] const scene::DayAlmanac& almanac(const scene::WorldClock& c) {
    const astro::UtcTime day = astro::local_midnight(c.utc, c.utc_offset_min);
    if (day != g_almanac.day_start || c.latitude_deg != g_almanac.latitude ||
        c.longitude_deg != g_almanac.longitude) {
        g_almanac = {day, c.latitude_deg, c.longitude_deg, scene::day_almanac(c)};
    }
    return g_almanac.almanac;
}

[[nodiscard]] std::string clock_text(const std::optional<astro::UtcTime>& t, int offset_min) {
    if (!t) return "—";
    // Rounded to the nearest minute, as almanacs print them.
    const astro::CivilTime c = astro::civil_from_utc(astro::add_seconds(*t, 30.0), offset_min);
    std::array<char, 16> buf{};
    std::snprintf(buf.data(), buf.size(), "%02d:%02d", c.hour, c.minute);
    return buf.data();
}

// Date row: YYYY-MM-DD text, applied on Enter; keeps the time of day.
void draw_date(scene::WorldClock& c) {
    const astro::CivilTime local = astro::civil_from_utc(c.utc, c.utc_offset_min);
    std::array<char, 16>   buf{};
    std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02d", local.year, local.month, local.day);
    std::string text = buf.data();
    if (ui::prop_text("Date", text, {.tooltip = "Local date, YYYY-MM-DD (years 1800..2200)"}, true)) {
        if (auto p = astro::parse_iso8601(text); p) {
            astro::CivilTime d = p->civil;
            d.hour = local.hour;
            d.minute = local.minute;
            d.second = local.second;
            c.utc = astro::utc_from_civil(d, c.utc_offset_min);
        }
    }
}

// Time-of-day scrubber, 00:00..24:00 local.
void draw_time_of_day(scene::WorldClock& c) {
    const astro::UtcTime midnight = astro::local_midnight(c.utc, c.utc_offset_min);
    float hours = static_cast<float>(static_cast<double>(c.utc.ns - midnight.ns) * 1e-9 / 3600.0);
    const astro::CivilTime local = astro::civil_from_utc(c.utc, c.utc_offset_min);
    std::array<char, 32>   label{};
    std::snprintf(label.data(), label.size(), "%02d:%02d:%02d", local.hour, local.minute,
                  static_cast<int>(local.second));
    ui::prop_begin("Time", "Local time of day; drag to scrub the sun across the sky", false, false);
    if (ui::slider_float("##time", &hours, 0.0F, 24.0F, label.data(), ImGuiSliderFlags_AlwaysClamp)) {
        c.utc = astro::add_seconds(midnight, std::min(static_cast<double>(hours), 24.0 - 1e-6) * 3600.0);
    }
    (void)ui::prop_end(false, false);
}

void draw_place(scene::WorldClock& c) {
    ui::prop_begin("Place", "Presets set latitude, longitude and the standard-time zone", false, false);
    if (ImGui::BeginCombo("##place", "Choose a city…")) {
        for (const Place& p : k_places) {
            if (ImGui::Selectable(p.name)) {
                // Keep the local time of day across the move.
                const astro::CivilTime local = astro::civil_from_utc(c.utc, c.utc_offset_min);
                c.latitude_deg = p.latitude;
                c.longitude_deg = p.longitude;
                c.utc_offset_min = p.utc_offset_min;
                c.utc = astro::utc_from_civil(local, c.utc_offset_min);
            }
        }
        ImGui::EndCombo();
    }
    (void)ui::prop_end(false, false);
    float lat = static_cast<float>(c.latitude_deg);
    float lon = static_cast<float>(c.longitude_deg);
    if (ui::prop_float("Latitude", lat, {.min = -90.0F, .max = 90.0F, .format = "%.4f°", .slider = true},
                       {.tooltip = "Degrees north (negative: south)"})) {
        c.latitude_deg = lat;
    }
    if (ui::prop_float("Longitude", lon, {.min = -180.0F, .max = 180.0F, .format = "%.4f°", .slider = true},
                       {.tooltip = "Degrees east (negative: west)"})) {
        c.longitude_deg = lon;
    }
    float offset_h = static_cast<float>(c.utc_offset_min) / 60.0F;
    if (ui::prop_float("UTC offset", offset_h, {.speed = 0.05F, .min = -14.0F, .max = 14.0F, .format = "%+.2f h"},
                       {.tooltip = "Time zone of the local time shown (the instant does not move)"})) {
        c.utc_offset_min = static_cast<int>(std::lround(offset_h * 4.0F)) * 15;
    }
    float north = static_cast<float>(c.north_offset_deg);
    if (ui::prop_float("North offset", north, {.min = -180.0F, .max = 180.0F, .format = "%.1f°", .slider = true},
                       {.reset = 0.0F, .tooltip = "World rotation of compass north (0: north is -Z, east +X)"})) {
        c.north_offset_deg = north;
    }
}

void draw_speed(scene::WorldClock& c) {
    ui::prop_begin("Speed", "Clock seconds per simulated second (the sim clock's pause and time "
                            "scale apply on top)", false, false);
    const bool paused = c.time_scale == 0.0;
    if (ui::toggle_button("pause", paused ? icon::play : icon::pause, paused,
                          paused ? "Run the clock" : "Freeze the clock")) {
        c.time_scale = paused ? 1.0 : 0.0;
    }
    for (std::size_t i = 0; i < k_speeds.size(); ++i) {
        ImGui::SameLine();
        if (ui::button(k_speed_labels[i], c.time_scale == k_speeds[i] ? ui::ButtonKind::primary
                                                                       : ui::ButtonKind::ghost)) {
            c.time_scale = k_speeds[i];
        }
        ui::item_tooltip(k_speed_tips[i]);
    }
    (void)ui::prop_end(false, false);
    float scale = static_cast<float>(c.time_scale);
    ui::prop_begin("Time scale", nullptr, true, c.time_scale != 1.0);
    if (ui::slider_float("##scale", &scale, 0.0F, 1.0e5F, "%.4gx",
                         ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
        c.time_scale = scale;
    }
    if (ui::prop_end(true, c.time_scale != 1.0)) c.time_scale = 1.0;
}

void draw_readouts(const entt::registry& r, entt::entity e, const scene::WorldClock& c) {
    const auto* sky = r.try_get<astro::SkyState>(e);
    if (sky == nullptr) {
        ui::prop_note("The clock is disabled: the sun keeps its own direction.");
        return;
    }
    ui::prop_separator("Sky");
    std::array<char, 96> buf{};
    std::snprintf(buf.data(), buf.size(), "az %.1f°  el %.1f°", sky->sun_azimuth_deg, sky->sun_elevation_deg);
    ui::prop_value("Sun", buf.data(), "Compass azimuth (0 north, 90 east) and apparent elevation", true);
    const scene::DayAlmanac& day = almanac(c);
    if (day.polar_day || day.polar_night) {
        ui::prop_value("Sunrise / set", day.polar_day ? "polar day (never sets)" : "polar night (never rises)");
    } else {
        const std::string v = clock_text(day.sunrise, c.utc_offset_min) + " / " +
                              clock_text(day.sunset, c.utc_offset_min) + "   noon " +
                              clock_text(day.solar_noon, c.utc_offset_min);
        ui::prop_value("Sunrise / set", v, "Upper limb on the horizon (USNO convention)", true);
    }
    ui::prop_value("Civil twilight",
                   clock_text(day.civil_dawn, c.utc_offset_min) + " / " + clock_text(day.civil_dusk, c.utc_offset_min),
                   "Sun's centre 6° below the horizon", true);
    std::snprintf(buf.data(), buf.size(), "%s, %.0f%% lit",
                  astro::moon_phase_name(sky->moon_elongation_deg, sky->moon_waxing),
                  sky->moon_illuminated_fraction * 100.0F);
    ui::prop_value("Moon phase", buf.data(), nullptr, true);
    std::snprintf(buf.data(), buf.size(), "az %.1f°  el %.1f°", sky->moon_azimuth_deg, sky->moon_elevation_deg);
    ui::prop_value("Moon", buf.data(), "Compass azimuth and apparent (topocentric) elevation", true);
    ui::prop_value("Moonrise / set",
                   clock_text(day.moonrise, c.utc_offset_min) + " / " + clock_text(day.moonset, c.utc_offset_min),
                   nullptr, true);
}

} // namespace

bool clock_drives_sun(const entt::registry& r) {
    const entt::entity e = scene::find_world_clock(r);
    if (e == entt::null) return false;
    const scene::WorldClock& c = r.get<scene::WorldClock>(e);
    return c.enabled && c.drive_sun;
}

void draw_world_clock(entt::registry& r, entt::entity e, scene::WorldClock& c) {
    const scene::WorldClock before = c;
    (void)ui::prop_bool("Enabled", c.enabled, {.tooltip = "Run the clock and derive the sky from it"});
    draw_date(c);
    draw_time_of_day(c);
    draw_speed(c);
    (void)ui::prop_bool("Fixed day", c.fixed_day, {.tooltip = "Loop the same day: the time wraps at midnight"});
    draw_place(c);
    ui::prop_separator("Sun light");
    (void)ui::prop_bool("Drive sun", c.drive_sun,
                        {.tooltip = "Point the scene's Directional Light from the computed Sun"});
    (void)ui::prop_float("Full-sun intensity", c.sun_intensity, {.speed = 0.05F, .min = 0.0F, .max = 100.0F},
                         {.tooltip = "Light intensity with the whole Sun above the horizon"});
    if (!astro::in_supported_range(c.utc)) c.utc = before.utc;
    // Edits show at once, also while the simulation is paused.
    if (before != c) scene::apply_world_clock(r);
    draw_readouts(r, e, c);
}

void draw_time_of_day_section(scene::Scene* scene) {
    if (scene == nullptr) return;
    entt::registry&    r = scene->registry();
    const entt::entity e = scene::find_world_clock(r);
    if (e == entt::null) {
        ui::text_secondary("No world clock: the sun stays where it is set.");
        if (ui::button(ui::label(icon::clock_3, "Add world clock").c_str(), ui::ButtonKind::primary)) {
            const entt::entity clock = scene->create_entity("world_clock");
            scene::WorldClock  c;
            for (const entt::entity l : r.view<scene::DirectionalLight>()) {
                c.sun_intensity = r.get<scene::DirectionalLight>(l).intensity; // keep the look
                break;
            }
            r.emplace<scene::WorldClock>(clock, c);
            scene::apply_world_clock(r);
        }
        return;
    }
    if (ui::begin_property_grid("world_clock")) {
        draw_world_clock(r, e, r.get<scene::WorldClock>(e));
        ui::end_property_grid();
    }
}

} // namespace engine::editor
