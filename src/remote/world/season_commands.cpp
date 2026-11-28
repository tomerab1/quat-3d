// season.get / season.set — the scene's SeasonState and Climate (ROADMAP
// R25.1), through the control surface shared with the Lua `season` table
// (engine/scene/season_api.hpp).

#include <nlohmann/json.hpp>

#include "engine/scene/scene.hpp"
#include "engine/scene/season.hpp"
#include "engine/scene/season_api.hpp"
#include "engine/scene/season_json.hpp"
#include "engine/weather/climate.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

RpcResult season_get(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    bool             climate = false;
    f.boolean("climate", climate);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    const entt::registry& r = ctx.scene->registry();
    Json                  out = scene::season_report(r);
    if (climate) {
        // The curves in use: the season entity's Climate, or the default.
        const entt::entity       e = scene::find_season(r);
        const weather::Climate*  c = e != entt::null ? r.try_get<weather::Climate>(e) : nullptr;
        out["climate_curves"] = scene::encode_climate(c != nullptr ? *c : weather::Climate{});
    }
    return out;
}

RpcResult season_set(Context& ctx, const Json& params) {
    if (auto ok = scene::set_season(*ctx.scene, params); !ok) return bad_params(ok.error().message);
    return scene::season_report(ctx.scene->registry());
}

} // namespace

void register_season_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("season.get",
                      "The scene's season: mode, year_phase (0 = local spring equinox), the "
                      "dominant season and weights, year_minutes / day_minutes, day length, the "
                      "climate now and the weather sequence's period. {climate: true} adds the "
                      "climate curves.",
                      [ctx](const Json& p) { return season_get(*ctx, p); });
    server.add_method("season.set",
                      "Set any of {mode, year_phase | season, year, phase_rate | year_minutes, "
                      "day_minutes, drive_clock_date, weather_auto, weather_seed, real_calendar, "
                      "climate, weather_preset, spin_up_s, enabled}; creates the season when the "
                      "scene has none. Returns season.get.",
                      [ctx](const Json& p) { return season_set(*ctx, p); });
}

} // namespace engine::remote::world
