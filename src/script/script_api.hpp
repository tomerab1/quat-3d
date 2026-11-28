#pragma once

// The engine API scripts call (G7), registered into a LuaVm. Private to
// engine_script; the Lua-facing reference is docs/scripting.md.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "engine/input/actions.hpp"
#include "engine/script/script_runtime.hpp"
#include "engine/core/log.hpp"
#include "lua_bind.hpp"

namespace engine::scene {
class Scene;
}

namespace engine::script::detail {

class LuaVm;

// An entity handle as scripts hold it (the Lua type `Entity`).
struct ScriptEntity {
    entt::entity id = entt::null;
    bool operator==(const ScriptEntity&) const = default;
};

// What the bindings reach. One per LuaVm; outlives it.
struct ApiContext {
    scene::Scene*  scene = nullptr; // null while only describing a script
    ScriptServices services;
    // The script whose code is running (log prefix).
    std::string current_path;
    // Destroys requested this phase; the runtime runs on_destroy, then
    // destroys them (Unity's deferred Destroy).
    std::vector<entt::entity> pending_destroy;
    // Clock (seconds of simulated time since play started).
    double        elapsed_s = 0.0;
    float         dt = 0.0F;
    float         fixed_dt = 1.0F / 60.0F;
    std::uint64_t frame = 0;
    // The input actions this phase reads (on_update: the update's sample;
    // on_fixed_update: one latched per fixed step) and the service that
    // produced them (input.push_context). Null outside a session update.
    const input::ActionSample* action_sample = nullptr;
    input::Actions*            action_service = nullptr;
    // The `self` table of another entity's running script (Entity:script()).
    std::function<sol::object(entt::entity)> instance_of;
};

// The live registry, or a failure naming why scripts cannot reach it.
[[nodiscard]] Result<entt::registry*> scene_registry(ApiContext& ctx);
// The registry, if `e` is live in it.
[[nodiscard]] Result<entt::registry*> live_entity(ApiContext& ctx, const ScriptEntity& e);
// "'name' (id 42)" for messages.
[[nodiscard]] std::string describe_entity(const entt::registry& registry, entt::entity e);
// The script-facing id: entt's full identifier (version included).
[[nodiscard]] std::uint32_t entity_id(entt::entity e);

// Logs `text` from the running script into the engine log ring.
void log_from_script(const ApiContext& ctx, core::LogLevel level, std::string_view text);

// JSON <-> Lua for the component codec. Arrays of three numbers become vec3;
// tables with keys 1..n become arrays (an empty nested table is an empty
// array), other tables objects. The top level of lua_to_json is an object.
[[nodiscard]] sol::object json_to_lua(lua_State* L, const nlohmann::json& value);
[[nodiscard]] Result<nlohmann::json> lua_to_json(const sol::object& value, bool top_object);

// Registration (one per API file).
void register_math(LuaVm& vm);
void register_entity(LuaVm& vm, ApiContext& ctx);
void register_scene(LuaVm& vm, ApiContext& ctx);
void register_services(LuaVm& vm, ApiContext& ctx);
void register_input(LuaVm& vm, ApiContext& ctx);
void register_terrain(LuaVm& vm, ApiContext& ctx); // G19.1 terrain.edit

// Transform helpers shared by the entity and scene bindings.
void refresh_world(entt::registry& registry, entt::entity e);
void teleport_physics(ApiContext& ctx, entt::registry& registry, entt::entity e);

} // namespace engine::script::detail
