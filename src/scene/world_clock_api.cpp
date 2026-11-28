// World clock control surface — see include/engine/scene/world_clock_api.hpp.

#include "engine/scene/world_clock_api.hpp"

#include <cmath>
#include <optional>
#include <string>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/astro/events.hpp"
#include "engine/astro/moon.hpp"
#include "engine/astro/sky_state.hpp"
#include "engine/astro/time.hpp"
#include "engine/core/json_fields.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/world_clock.hpp"
#include "engine/scene/world_clock_json.hpp"

namespace engine::scene {

namespace {

using Json = nlohmann::json;

constexpr std::size_t k_max_time_length = 32;

[[nodiscard]] Json local_time(const std::optional<astro::UtcTime>& t, int utc_offset_min) {
    return t ? Json(astro::format_iso8601(*t, utc_offset_min)) : Json(nullptr);
}

[[nodiscard]] Json vec3_json(const glm::vec3& v) { return Json::array({v.x, v.y, v.z}); }

// The first DirectionalLight (the one the renderer lights with), or null.
[[nodiscard]] entt::entity find_sun(const entt::registry& r) {
    for (const entt::entity e : r.view<const DirectionalLight>()) return e;
    return entt::null;
}

} // namespace

Json world_clock_report(const entt::registry& r) {
    const entt::entity e = find_world_clock(r);
    const WorldClock   c = e != entt::null ? r.get<WorldClock>(e) : WorldClock{};

    Json out = encode_world_clock(c);
    if (e == entt::null) out["enabled"] = false; // nothing runs without a clock
    out["in_scene"] = e != entt::null;
    out["entity"] = e != entt::null ? Json(entt::to_integral(e)) : Json(nullptr);
    out["utc"] = astro::format_iso8601(c.utc, 0);

    const astro::SkyState sky = astro::compute_sky_state(c.utc, observer_of(c));
    const DayAlmanac      day = day_almanac(c);
    out["julian_date"] = sky.julian_date_ut;
    out["local_sidereal_time_deg"] = sky.local_sidereal_time_deg;

    Json sun = {{"azimuth_deg", sky.sun_azimuth_deg},
                {"elevation_deg", sky.sun_elevation_deg},
                {"direction", vec3_json(sky.sun_direction)},
                {"illuminance_lux", sky.sun_illuminance_lux},
                {"visible_fraction", sky.sun_disc_visible},
                {"sunrise", local_time(day.sunrise, c.utc_offset_min)},
                {"sunset", local_time(day.sunset, c.utc_offset_min)},
                {"solar_noon", local_time(day.solar_noon, c.utc_offset_min)},
                {"civil_dawn", local_time(day.civil_dawn, c.utc_offset_min)},
                {"civil_dusk", local_time(day.civil_dusk, c.utc_offset_min)},
                {"polar", day.polar_day ? Json("day") : (day.polar_night ? Json("night") : Json(nullptr))}};
    if (day.sunrise && day.sunset && *day.sunset > *day.sunrise) {
        sun["day_length_h"] = static_cast<double>(day.sunset->ns - day.sunrise->ns) * 1e-9 / 3600.0;
    }
    out["sun"] = std::move(sun);
    out["moon"] = {{"azimuth_deg", sky.moon_azimuth_deg},
                   {"elevation_deg", sky.moon_elevation_deg},
                   {"direction", vec3_json(sky.moon_direction)},
                   {"illuminated_fraction", sky.moon_illuminated_fraction},
                   {"phase_angle_deg", sky.moon_phase_angle_deg},
                   {"phase", astro::moon_phase_name(sky.moon_elongation_deg, sky.moon_waxing)},
                   {"illuminance_lux", sky.moon_illuminance_lux},
                   {"distance_km", sky.moon_distance_km},
                   {"moonrise", local_time(day.moonrise, c.utc_offset_min)},
                   {"moonset", local_time(day.moonset, c.utc_offset_min)}};
    return out;
}

std::expected<void, core::Error> set_world_clock(Scene& scene, const Json& params) {
    entt::registry&    r = scene.registry();
    const entt::entity existing = find_world_clock(r);
    WorldClock         c = existing != entt::null ? r.get<WorldClock>(existing) : WorldClock{};
    if (existing == entt::null) {
        // A new clock keeps the look: full sun at the light's current intensity.
        if (const entt::entity sun = find_sun(r); sun != entt::null) {
            c.sun_intensity = r.get<DirectionalLight>(sun).intensity;
        }
    }

    core::JsonFields f(params, "");
    decode_world_clock(f, c);
    std::string local;
    const bool  has_local = f.has("local_time");
    f.string("local_time", local, k_max_time_length);
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());

    if (has_local) {
        const auto seconds = astro::parse_time_of_day(local);
        if (!seconds) return std::unexpected(core::Error{"local_time: " + seconds.error()});
        c.utc = astro::add_seconds(astro::local_midnight(c.utc, c.utc_offset_min), *seconds);
    }
    if (!astro::in_supported_range(c.utc)) {
        return std::unexpected(core::Error{
            "datetime: the resulting UTC instant " + astro::format_iso8601(c.utc, 0) +
            " is outside the supported years " + std::to_string(astro::k_min_year) + ".." +
            std::to_string(astro::k_max_year)});
    }

    entt::entity e = existing;
    if (e == entt::null) e = scene.create_entity("world_clock");
    r.emplace_or_replace<WorldClock>(e, c);
    // A clock that drives the sun needs one to drive.
    if (c.enabled && c.drive_sun && find_sun(r) == entt::null) {
        r.emplace<DirectionalLight>(scene.create_entity("sun"));
    }
    apply_world_clock(r);
    return {};
}

std::expected<void, core::Error> skip_world_clock(entt::registry& r, double hours) {
    if (!std::isfinite(hours) || std::abs(hours) > k_max_clock_skip_hours) {
        return std::unexpected(core::Error{"hours: expected a number in [-87600, 87600]"});
    }
    const entt::entity e = find_world_clock(r);
    if (e == entt::null) {
        return std::unexpected(core::Error{"the scene has no world clock: create one with time.set first"});
    }
    WorldClock& c = r.get<WorldClock>(e);
    c.utc = advance_clock_time(c, hours * 3600.0);
    apply_world_clock(r);
    return {};
}

} // namespace engine::scene
