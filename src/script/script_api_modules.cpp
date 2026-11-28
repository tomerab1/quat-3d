// Native Lua modules of game modules (ROADMAP G17.1): require("<name>")
// returns a read-only table of the module's functions. Each call converts its
// Lua arguments to one JSON array, runs the native function inside
// checked_call (so a failure raises a Lua error only after the C++ frames
// returned, see lua_bind.hpp) and converts the JSON result back.

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "lua_vm.hpp"
#include "script_api.hpp"

namespace engine::script::detail {

void register_modules(LuaVm& vm, std::span<const LuaModuleDesc> modules) {
    lua_State* L = vm.state();
    for (const LuaModuleDesc& module : modules) {
        sol::table t = vm.lua().create_table();
        for (const LuaFunctionDesc& f : module.functions) {
            t.set(f.name, bind(L, [fn = f.fn, where = module.name + "." + f.name](
                                      sol::variadic_args args, sol::this_state s) -> Result<sol::object> {
                      nlohmann::json list = nlohmann::json::array();
                      for (const sol::stack_proxy arg : args) {
                          auto converted = lua_to_json(sol::object(arg), false);
                          if (!converted.value) return fail(where + ": " + converted.value.error().message);
                          list.push_back(std::move(*converted.value));
                      }
                      auto result = fn(list);
                      if (!result) return fail(where + ": " + result.error().message);
                      return json_to_lua(s, *result);
                  }));
        }
        vm.preload(module.name, t);
    }
}

} // namespace engine::script::detail
