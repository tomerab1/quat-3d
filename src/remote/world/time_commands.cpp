// time.get / time.set / time.skip — the scene's WorldClock (ROADMAP R22.1).

#include <cmath>
#include <optional>
#include <string>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/astro/events.hpp"
#include "engine/astro/moon.hpp"
#include "engine/astro/sky_state.hpp"
#include "engine/astro/time.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/world_clock.hpp"
#include "engine/scene/world_clock_json.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr double k_max_skip_hours = 87600.0; // ten years
constexpr std::size_t k_max_time_length = 32;

[[nodiscard]] Json local_time(const std::optional<astro::UtcTime>& t, int utc_offset_min) {
    return t ? Json(astro::format_iso8601(*t, utc_offset_min)) : Json(nullptr);
}

[[nodiscard]] Json time_state(const entt::registry& r) {
    const entt::entity     e = scene::find_world_clock(r);
    const scene::WorldClock c = e != entt::null ? r.get<scene::WorldClock>(e) : scene::WorldClock{};

    Json out = scene::encode_world_clock(c);
    if (e == entt::null) out["enabled"] = false; // nothing runs without a clock
    out["in_scene"] = e != entt::null;
    out["entity"] = e != entt::null ? entity_json(e) : Json(nullptr);
    out["utc"] = astro::format_iso8601(c.utc, 0);

    const astro::SkyState   sky = astro::compute_sky_state(c.utc, scene::observer_of(c));
    const scene::DayAlmanac day = scene::day_almanac(c);
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

RpcResult time_get(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    return time_state(ctx.scene->registry());
}

// The first DirectionalLight (the one the renderer lights with), or null.
[[nodiscard]] entt::entity find_sun(const entt::registry& r) {
    for (const entt::entity e : r.view<const scene::DirectionalLight>()) return e;
    return entt::null;
}

RpcResult time_set(Context& ctx, const Json& params) {
    entt::registry&    r = ctx.scene->registry();
    const entt::entity existing = scene::find_world_clock(r);
    scene::WorldClock  c = existing != entt::null ? r.get<scene::WorldClock>(existing) : scene::WorldClock{};
    if (existing == entt::null) {
        // A new clock keeps the look: full sun at the light's current intensity.
        if (const entt::entity sun = find_sun(r); sun != entt::null) {
            c.sun_intensity = r.get<scene::DirectionalLight>(sun).intensity;
        }
    }

    core::JsonFields f(params, "");
    scene::decode_world_clock(f, c);
    std::string local;
    const bool  has_local = f.has("local_time");
    f.string("local_time", local, k_max_time_length);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());

    if (has_local) {
        const auto seconds = astro::parse_time_of_day(local);
        if (!seconds) return bad_params("local_time: " + seconds.error());
        c.utc = astro::add_seconds(astro::local_midnight(c.utc, c.utc_offset_min), *seconds);
    }
    if (!astro::in_supported_range(c.utc)) {
        return bad_params("datetime: the resulting UTC instant " + astro::format_iso8601(c.utc, 0) +
                          " is outside the supported years " + std::to_string(astro::k_min_year) + ".." +
                          std::to_string(astro::k_max_year));
    }

    entt::entity e = existing;
    if (e == entt::null) e = ctx.scene->create_entity("world_clock");
    r.emplace_or_replace<scene::WorldClock>(e, c);
    // A clock that drives the sun needs one to drive.
    if (c.enabled && c.drive_sun && find_sun(r) == entt::null) {
        r.emplace<scene::DirectionalLight>(ctx.scene->create_entity("sun"));
    }
    scene::apply_world_clock(r);
    return time_state(r);
}

RpcResult time_skip(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    double           hours = 0.0;
    const bool       has_hours = f.has("hours");
    f.number("hours", hours, -k_max_skip_hours, k_max_skip_hours);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!has_hours) return bad_params("hours: required (a number of hours to move the clock; negative goes back)");
    const entt::entity e = scene::find_world_clock(r);
    if (e == entt::null) {
        return rpc_fail(rpc_code::not_found, "the scene has no world clock: create one with time.set first");
    }
    scene::WorldClock& c = r.get<scene::WorldClock>(e);
    c.utc = scene::advance_clock_time(c, hours * 3600.0);
    scene::apply_world_clock(r);
    return time_state(r);
}

} // namespace

void register_time_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("time.get",
                      "The scene's world clock (date / time / place / speed), the Sun and Moon "
                      "(compass azimuth, elevation, world direction, illuminance, phase) and the "
                      "local day's sunrise / sunset / solar noon / twilight / moonrise / moonset.",
                      [ctx](const Json& p) { return time_get(*ctx, p); });
    server.add_method("time.set",
                      "Set any of {datetime (ISO-8601), local_time (hh:mm[:ss]), utc_offset_h, "
                      "latitude_deg, longitude_deg, north_offset_deg, time_scale, fixed_day, "
                      "enabled, drive_sun, sun_intensity}; creates the clock when the scene has "
                      "none. Returns time.get.",
                      [ctx](const Json& p) { return time_set(*ctx, p); });
    server.add_method("time.skip",
                      "Move the world clock by {hours} (negative goes back; wraps within the day "
                      "under fixed_day). Returns time.get.",
                      [ctx](const Json& p) { return time_skip(*ctx, p); });
}

} // namespace engine::remote::world
