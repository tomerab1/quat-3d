// The Lua `season` table (ROADMAP R25.1): the scene's season and climate,
// through the same control surface as the remote season.* commands
// (engine/scene/season_api.hpp) — see docs/scripting.md.

#include <nlohmann/json.hpp>

#include "engine/scene/scene.hpp"
#include "engine/scene/season_api.hpp"
#include "lua_vm.hpp"
#include "script_api.hpp"

namespace engine::script::detail {

void register_season(LuaVm& vm, ApiContext& c) {
    ApiContext* ctx = &c;
    lua_State*  L = vm.state();
    sol::table  t(L, sol::create);
    t.set("get", bind(L, [ctx](sol::this_state s) -> Result<sol::object> {
              auto r = scene_registry(*ctx);
              if (!r.value) return std::unexpected(r.value.error());
              return json_to_lua(s.lua_state(), scene::season_report(**r.value));
          }));
    t.set("set", bind(L, [ctx](const sol::table& spec, sol::this_state s) -> Result<sol::object> {
              auto r = scene_registry(*ctx);
              if (!r.value) return std::unexpected(r.value.error());
              auto json = lua_to_json(spec, true);
              if (!json.value) return fail("season.set: " + json.value.error().message);
              if (auto ok = scene::set_season(*ctx->scene, *json.value); !ok) {
                  return fail("season.set: " + ok.error().message);
              }
              return json_to_lua(s.lua_state(), scene::season_report(**r.value));
          }));
    vm.expose("season", t);
}

} // namespace engine::script::detail
