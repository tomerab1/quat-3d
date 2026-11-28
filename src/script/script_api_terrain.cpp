// Lua `terrain.edit` (G19.1): the terrain's edit stamps from scripts, through
// the same scene::TerrainEdits service and stamp codec as the remote
// terrain.edit.* commands. See docs/scripting.md.

#include <string>

#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/scene/terrain_edits.hpp"
#include "lua_vm.hpp"
#include "script_api.hpp"

namespace engine::script::detail {

namespace {

// A stamp table ({shape = "circle", points = {{x, z}}, ...}) merged onto
// `stamp` through the codec (strict: unknown keys are named).
[[nodiscard]] Result<void> read_stamp(const sol::object& table, std::string_view where,
                                      terrain::TerrainStamp& stamp) {
    if (table.get_type() != sol::type::table) {
        return fail(std::string(where) + ": expected a stamp table");
    }
    auto json = lua_to_json(table, true);
    if (!json.value) return std::unexpected(json.value.error());
    core::JsonFields f(*json.value, std::string(where));
    scene::decode_stamp(f, stamp);
    if (auto ok = f.finish(); !ok) return fail(ok.error().message);
    return {};
}

} // namespace

void register_terrain(LuaVm& vm, ApiContext& c) {
    lua_State* L = vm.state();
    ApiContext* ctx = &c;
    sol::table edit(L, sol::create);
    edit.set("add", bind(L, [ctx](const sol::object& table,
                                  sol::optional<std::uint64_t> before) -> Result<std::uint64_t> {
                 auto r = scene_registry(*ctx);
                 if (!r.value) return std::unexpected(r.value.error());
                 terrain::TerrainStamp stamp;
                 if (auto ok = read_stamp(table, "terrain.edit.add", stamp); !ok.value) {
                     return std::unexpected(ok.value.error());
                 }
                 if (stamp.id != 0) {
                     return fail("terrain.edit.add: id is assigned (use terrain.edit.update)");
                 }
                 scene::TerrainEdits edits(**r.value);
                 auto id = edits.add(stamp, before ? std::optional(*before) : std::nullopt);
                 if (!id) return fail("terrain.edit.add: " + id.error().message);
                 return *id;
             }));
    edit.set("update", bind(L, [ctx](std::uint64_t id, const sol::object& table) -> Result<void> {
                 auto r = scene_registry(*ctx);
                 if (!r.value) return std::unexpected(r.value.error());
                 scene::TerrainEdits edits(**r.value);
                 std::optional<terrain::TerrainStamp> stamp = edits.get(id);
                 if (!stamp) {
                     return fail("terrain.edit.update: no stamp with id " + std::to_string(id));
                 }
                 const std::uint64_t keep = stamp->id;
                 if (auto ok = read_stamp(table, "terrain.edit.update", *stamp); !ok.value) {
                     return std::unexpected(ok.value.error());
                 }
                 stamp->id = keep;
                 if (auto ok = edits.update(id, *stamp); !ok) {
                     return fail("terrain.edit.update: " + ok.error().message);
                 }
                 return {};
             }));
    edit.set("remove", bind(L, [ctx](std::uint64_t id) -> Result<bool> {
                 auto r = scene_registry(*ctx);
                 if (!r.value) return std::unexpected(r.value.error());
                 return scene::TerrainEdits(**r.value).remove(id).has_value();
             }));
    edit.set("clear", bind(L, [ctx]() -> Result<std::size_t> {
                 auto r = scene_registry(*ctx);
                 if (!r.value) return std::unexpected(r.value.error());
                 return scene::TerrainEdits(**r.value).clear();
             }));
    edit.set("list", bind(L, [ctx](sol::this_state s) -> Result<sol::object> {
                 auto r = scene_registry(*ctx);
                 if (!r.value) return std::unexpected(r.value.error());
                 nlohmann::json out = nlohmann::json::array();
                 for (const terrain::TerrainStamp& st : scene::TerrainEdits(**r.value).list()) {
                     out.push_back(scene::encode_stamp(st));
                 }
                 return json_to_lua(s.lua_state(), out);
             }));
    sol::table t(L, sol::create);
    t["edit"] = edit;
    vm.expose("terrain", t);
}

} // namespace engine::script::detail
