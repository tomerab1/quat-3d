// time.get / time.set / time.skip — the scene's WorldClock (ROADMAP R22.1),
// through the control surface shared with the Lua `time` table
// (engine/scene/world_clock_api.hpp).

#include <nlohmann/json.hpp>

#include "engine/scene/scene.hpp"
#include "engine/scene/world_clock.hpp"
#include "engine/scene/world_clock_api.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

RpcResult time_get(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    return scene::world_clock_report(ctx.scene->registry());
}

RpcResult time_set(Context& ctx, const Json& params) {
    if (auto ok = scene::set_world_clock(*ctx.scene, params); !ok) return bad_params(ok.error().message);
    return scene::world_clock_report(ctx.scene->registry());
}

RpcResult time_skip(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    double           hours = 0.0;
    const bool       has_hours = f.has("hours");
    f.number("hours", hours, -scene::k_max_clock_skip_hours, scene::k_max_clock_skip_hours);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!has_hours) return bad_params("hours: required (a number of hours to move the clock; negative goes back)");
    if (scene::find_world_clock(r) == entt::null) {
        return rpc_fail(rpc_code::not_found, "the scene has no world clock: create one with time.set first");
    }
    if (auto ok = scene::skip_world_clock(r, hours); !ok) return bad_params(ok.error().message);
    return scene::world_clock_report(r);
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
