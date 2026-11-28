// log / print, time, physics and audio for scripts, plus the helpers the
// bindings share — see docs/scripting.md.

#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/dynamic_mesh_api.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/world_clock_api.hpp"
#include "lua_vm.hpp"
#include "script_api.hpp"

namespace engine::script::detail {

namespace {

constexpr std::size_t k_max_log_bytes = 4096;

// print(...) / log.<level>(...): tostring of each argument, tab separated.
// A plain C function (it calls __tostring metamethods, which may raise).
int lua_log(lua_State* L) {
    const auto* ctx = static_cast<const ApiContext*>(lua_touserdata(L, lua_upvalueindex(1)));
    const auto level = static_cast<core::LogLevel>(lua_tointeger(L, lua_upvalueindex(2)));
    const int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    std::size_t len = 0;
    const char* text = lua_tolstring(L, -1, &len);
    log_from_script(*ctx, level, std::string_view(text, std::min(len, k_max_log_bytes)));
    return 0;
}

void push_logger(lua_State* L, ApiContext* ctx, core::LogLevel level) {
    lua_pushlightuserdata(L, ctx);
    lua_pushinteger(L, static_cast<lua_Integer>(level));
    lua_pushcclosure(L, &lua_log, 2);
}

[[nodiscard]] glm::quat rotation_of(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) {
        const float len = glm::length(r[c]);
        r[c] = len > 1e-12F ? r[c] / len : glm::vec3(0.0F);
    }
    return glm::normalize(glm::quat_cast(r));
}

void register_log(LuaVm& vm, ApiContext& ctx) {
    lua_State* L = vm.state();
    sol::table log(L, sol::create);
    for (const auto& [name, level] :
         {std::pair{"debug", core::LogLevel::debug}, std::pair{"info", core::LogLevel::info},
          std::pair{"warn", core::LogLevel::warn}, std::pair{"error", core::LogLevel::error}}) {
        push_logger(L, &ctx, level);
        log[name] = sol::object(L, -1);
        lua_pop(L, 1);
    }
    vm.expose("log", log);
    push_logger(L, &ctx, core::LogLevel::info);
    vm.expose("print", sol::object(L, -1));
    lua_pop(L, 1);
}

void register_time(LuaVm& vm, ApiContext& c) {
    ApiContext* ctx = &c;
    sol::table t(vm.state(), sol::create);
    t.set_function("delta", [ctx] { return ctx->dt; });
    t.set_function("elapsed", [ctx] { return ctx->elapsed_s; });
    t.set_function("frame", [ctx] { return ctx->frame; });
    t.set_function("fixed_delta", [ctx] { return ctx->fixed_dt; });
    // The world clock (R22.1): the same surface as the remote time.* commands.
    lua_State* L = vm.state();
    t.set("clock", bind(L, [ctx](sol::this_state s) -> Result<sol::object> {
              auto r = scene_registry(*ctx);
              if (!r.value) return std::unexpected(r.value.error());
              return json_to_lua(s.lua_state(), scene::world_clock_report(**r.value));
          }));
    t.set("set_clock", bind(L, [ctx](const sol::table& spec, sol::this_state s) -> Result<sol::object> {
              auto r = scene_registry(*ctx);
              if (!r.value) return std::unexpected(r.value.error());
              auto json = lua_to_json(spec, true);
              if (!json.value) return fail("set_clock: " + json.value.error().message);
              if (auto ok = scene::set_world_clock(*ctx->scene, *json.value); !ok) {
                  return fail("set_clock: " + ok.error().message);
              }
              return json_to_lua(s.lua_state(), scene::world_clock_report(**r.value));
          }));
    t.set("skip", bind(L, [ctx](double hours, sol::this_state s) -> Result<sol::object> {
              auto r = scene_registry(*ctx);
              if (!r.value) return std::unexpected(r.value.error());
              if (auto ok = scene::skip_world_clock(**r.value, hours); !ok) {
                  return fail("skip: " + ok.error().message);
              }
              return json_to_lua(s.lua_state(), scene::world_clock_report(**r.value));
          }));
    vm.expose("time", t);
}

// geometry (R23.2): runtime procedural geometry, the same JSON surface as the
// remote geometry.* / mesh.dynamic.* methods (scene/dynamic_mesh_api.hpp):
// tables in, tables out.
void register_geometry(LuaVm& vm, ApiContext& c) {
    ApiContext* ctx = &c;
    lua_State* L = vm.state();
    sol::table g(L, sol::create);
    using Api = std::expected<nlohmann::json, core::Error> (*)(scene::DynamicMeshStore&, const nlohmann::json&);
    const auto store_call = [ctx, L](const char* name, Api api) {
        return bind(L, [ctx, name, api](const sol::table& params, sol::this_state s) -> Result<sol::object> {
            if (ctx->services.dynamic_meshes == nullptr) {
                return fail(std::string(name) + ": no dynamic mesh store in this build");
            }
            auto json = lua_to_json(params, true);
            if (!json.value) return fail(std::string(name) + ": " + json.value.error().message);
            auto out = api(*ctx->services.dynamic_meshes, *json.value);
            if (!out) return fail(std::string(name) + ": " + out.error().message);
            return json_to_lua(s.lua_state(), *out);
        });
    };
    g.set("generate", bind(L, [ctx](const sol::table& params, sol::this_state s) -> Result<sol::object> {
              auto json = lua_to_json(params, true);
              if (!json.value) return fail("generate: " + json.value.error().message);
              auto out = scene::geometry_generate(ctx->services.dynamic_meshes, *json.value);
              if (!out) return fail("generate: " + out.error().message);
              return json_to_lua(s.lua_state(), *out);
          }));
    g.set("polygon", bind(L, [](const sol::table& params, sol::this_state s) -> Result<sol::object> {
              auto json = lua_to_json(params, true);
              if (!json.value) return fail("polygon: " + json.value.error().message);
              auto out = scene::geometry_polygon(*json.value);
              if (!out) return fail("polygon: " + out.error().message);
              return json_to_lua(s.lua_state(), *out);
          }));
    g.set("set_mesh", store_call("set_mesh", &scene::dynamic_mesh_set));
    g.set("update_vertices", store_call("update_vertices", &scene::dynamic_mesh_update));
    g.set("remove_mesh", store_call("remove_mesh", &scene::dynamic_mesh_remove));
    g.set("mesh_info", store_call("mesh_info", [](scene::DynamicMeshStore& st, const nlohmann::json& p) {
              return scene::dynamic_mesh_get(st, p);
          }));
    vm.expose("geometry", g);
}

void register_physics(LuaVm& vm, ApiContext& c) {
    lua_State* L = vm.state();
    ApiContext* ctx = &c;
    sol::table t(L, sol::create);
    t.set("raycast", bind(L, [ctx](const glm::vec3& origin, const glm::vec3& direction,
                                   sol::optional<float> max_distance, sol::optional<bool> sensors,
                                   sol::this_state s) -> Result<sol::object> {
              if (ctx->services.physics == nullptr) {
                  return fail("physics.raycast: no physics world is running (play mode)");
              }
              if (glm::length(direction) < 1e-6F) return fail("physics.raycast: direction is zero");
              const float dist = max_distance.value_or(1000.0F);
              if (!(dist > 0.0F)) return fail("physics.raycast: max_distance must be > 0");
              const auto hit =
                  ctx->services.physics->cast_ray(origin, direction, dist, sensors.value_or(false));
              if (!hit) return sol::make_object(s.lua_state(), sol::lua_nil);
              sol::table out(s.lua_state(), sol::create);
              out["point"] = hit->point;
              out["normal"] = hit->normal;
              out["distance"] = hit->distance;
              out["sensor"] = hit->sensor;
              const auto e = static_cast<entt::entity>(hit->owner);
              if (hit->owner != physics::PhysicsWorld::no_owner && ctx->scene != nullptr &&
                  ctx->scene->registry().valid(e)) {
                  out["entity"] = ScriptEntity{e};
              }
              return sol::object(out);
          }));
    vm.expose("physics", t);
}

void register_audio(LuaVm& vm, ApiContext& c) {
    lua_State* L = vm.state();
    ApiContext* ctx = &c;
    sol::table t(L, sol::create);
    const auto command = [ctx](scene::AudioCommand cmd) {
        return [ctx, cmd](const ScriptEntity& e) -> Result<void> {
            auto r = live_entity(*ctx, e);
            if (!r.value) return std::unexpected(r.value.error());
            auto* src = (*r.value)->try_get<scene::AudioSource>(e.id);
            if (src == nullptr) {
                return fail("entity " + describe_entity(**r.value, e.id) + " has no audio_source");
            }
            src->command = cmd;
            return {};
        };
    };
    t.set("play", bind(L, command(scene::AudioCommand::play)));
    t.set("stop", bind(L, command(scene::AudioCommand::stop)));
    t.set("pause", bind(L, command(scene::AudioCommand::pause)));
    t.set("resume", bind(L, command(scene::AudioCommand::resume)));
    vm.expose("audio", t);
}

} // namespace

Result<entt::registry*> scene_registry(ApiContext& ctx) {
    if (ctx.scene == nullptr) {
        return fail("the scene is not available while a script loads; use it from on_start / "
                    "on_update");
    }
    return &ctx.scene->registry();
}

Result<entt::registry*> live_entity(ApiContext& ctx, const ScriptEntity& e) {
    auto r = scene_registry(ctx);
    if (!r.value) return r;
    if (!(*r.value)->valid(e.id)) {
        return fail("entity " + std::to_string(entity_id(e.id)) + " no longer exists");
    }
    return r;
}

std::uint32_t entity_id(entt::entity e) { return static_cast<std::uint32_t>(entt::to_integral(e)); }

std::string describe_entity(const entt::registry& registry, entt::entity e) {
    const auto* n = registry.valid(e) ? registry.try_get<scene::Name>(e) : nullptr;
    const std::string id = std::to_string(entity_id(e));
    return n != nullptr && !n->value.empty() ? "'" + n->value + "' (id " + id + ")" : id;
}

void log_from_script(const ApiContext& ctx, core::LogLevel level, std::string_view text) {
    if (ctx.current_path.empty()) {
        core::log_message(level, "script", text);
    } else {
        core::log_message(level, "script", ctx.current_path + ": " + std::string(text));
    }
}

void refresh_world(entt::registry& r, entt::entity root) {
    std::vector<entt::entity> stack{root};
    std::size_t guard = 0;
    while (!stack.empty() && guard++ < 100'000) {
        const entt::entity e = stack.back();
        stack.pop_back();
        auto* t = r.try_get<scene::Transform>(e);
        if (t == nullptr) continue;
        glm::mat4 parent_world(1.0F);
        if (const auto* p = r.try_get<scene::Parent>(e);
            p != nullptr && p->entity != entt::null && r.valid(p->entity)) {
            if (const auto* pt = r.try_get<scene::Transform>(p->entity)) parent_world = pt->world;
        }
        t->world = parent_world * t->local;
        if (const auto* ch = r.try_get<scene::Children>(e)) {
            for (const entt::entity c : ch->entities) {
                if (r.valid(c)) stack.push_back(c);
            }
        }
    }
}

void teleport_physics(ApiContext& ctx, entt::registry& r, entt::entity e) {
    physics::PhysicsWorld* world = ctx.services.physics;
    const auto* t = r.try_get<scene::Transform>(e);
    if (world == nullptr || t == nullptr) return;
    const glm::vec3 position(t->world[3]);
    const glm::quat rotation = rotation_of(t->world);
    if (const auto* rb = r.try_get<scene::RigidBody>(e);
        rb != nullptr && rb->body != physics::PhysicsWorld::invalid_body) {
        glm::vec3 offset(0.0F);
        if (const auto* col = r.try_get<scene::Collider>(e)) {
            const glm::vec3 scale(glm::length(glm::vec3(t->world[0])),
                                  glm::length(glm::vec3(t->world[1])),
                                  glm::length(glm::vec3(t->world[2])));
            offset = rotation * (scale * col->offset);
        }
        world->set_body_transform(rb->body, position + offset, rotation);
    }
    if (const auto* cc = r.try_get<scene::CharacterController>(e);
        cc != nullptr && cc->character != physics::PhysicsWorld::invalid_body) {
        world->character_set_position(cc->character, position);
    }
}

void register_services(LuaVm& vm, ApiContext& ctx) {
    register_log(vm, ctx);
    register_time(vm, ctx);
    register_geometry(vm, ctx);
    register_physics(vm, ctx);
    register_audio(vm, ctx);
}

} // namespace engine::script::detail
