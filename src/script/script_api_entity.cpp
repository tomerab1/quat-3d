// The Entity type scripts use — see docs/scripting.md ("Entity").

#include <algorithm>
#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/navigation.hpp"
#include "engine/scene/scene.hpp"
#include "lua_vm.hpp"
#include "script_api.hpp"

namespace engine::script::detail {

namespace {

using glm::quat;
using glm::vec3;
using scene::Transform;
constexpr std::uint32_t k_invalid = physics::PhysicsWorld::invalid_body;

struct Trs {
    vec3 position{0.0F};
    quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    vec3 scale{1.0F};
};

[[nodiscard]] Trs decompose(const glm::mat4& m) {
    Trs t;
    t.position = vec3(m[3]);
    t.scale = {glm::length(vec3(m[0])), glm::length(vec3(m[1])), glm::length(vec3(m[2]))};
    glm::mat3 r(1.0F);
    for (int c = 0; c < 3; ++c) {
        r[c] = t.scale[c] > 1e-12F ? vec3(m[c]) / t.scale[c] : vec3(0.0F);
    }
    t.rotation = glm::normalize(glm::quat_cast(r));
    return t;
}

[[nodiscard]] glm::mat4 compose(const Trs& t) {
    return glm::translate(glm::mat4(1.0F), t.position) * glm::mat4_cast(t.rotation) *
           glm::scale(glm::mat4(1.0F), t.scale);
}

[[nodiscard]] Result<Transform*> transform_of(ApiContext& ctx, const ScriptEntity& e) {
    auto r = live_entity(ctx, e);
    if (!r.value) return std::unexpected(r.value.error());
    auto* t = (*r.value)->try_get<Transform>(e.id);
    if (t == nullptr) return fail("entity " + describe_entity(**r.value, e.id) + " has no transform");
    return t;
}

// Moves `e` to world pose `world`, keeps its subtree attached and teleports
// its physics presence.
void place(ApiContext& ctx, entt::registry& r, entt::entity e, const glm::mat4& world) {
    scene::set_world_transform(r, e, world);
    refresh_world(r, e);
    teleport_physics(ctx, r, e);
}

template <typename T>
[[nodiscard]] Result<T*> component_of(ApiContext& ctx, const ScriptEntity& e, const char* what,
                                      const char* hint) {
    auto r = live_entity(ctx, e);
    if (!r.value) return std::unexpected(r.value.error());
    T* c = (*r.value)->try_get<T>(e.id);
    if (c == nullptr) {
        return fail("entity " + describe_entity(**r.value, e.id) + " has no " + what + " (" +
                    hint + ")");
    }
    return c;
}

[[nodiscard]] Result<physics::PhysicsWorld*> physics_of(ApiContext& ctx) {
    if (ctx.services.physics == nullptr) {
        return fail("no physics world is running (physics exists in play mode)");
    }
    return ctx.services.physics;
}

// The dynamic body of `e` (created now if the physics step has not yet).
[[nodiscard]] Result<std::uint32_t> dynamic_body(ApiContext& ctx, const ScriptEntity& e) {
    auto world = physics_of(ctx);
    if (!world.value) return std::unexpected(world.value.error());
    auto rb = component_of<scene::RigidBody>(ctx, e, "rigidbody", "add a dynamic rigidbody + collider");
    if (!rb.value) return std::unexpected(rb.value.error());
    entt::registry& r = ctx.scene->registry();
    if ((*rb.value)->motion != scene::BodyMotion::dynamic) {
        return fail("entity " + describe_entity(r, e.id) +
                    " is not dynamic (impulses and velocities move dynamic bodies)");
    }
    const std::uint32_t body = scene::ensure_rigid_body(r, **world.value, e.id);
    if (body == k_invalid) {
        return fail("entity " + describe_entity(r, e.id) + " has no physics body (a collider is missing)");
    }
    return body;
}

void bind_transform(sol::usertype<ScriptEntity>& t, lua_State* L, ApiContext& c) {
    ApiContext* ctx = &c;
    t.set("position", bind(L, [ctx](const ScriptEntity& e) -> Result<vec3> {
              auto tf = transform_of(*ctx, e);
              if (!tf.value) return std::unexpected(tf.value.error());
              return vec3((*tf.value)->world[3]);
          }));
    t.set("set_position", bind(L, [ctx](const ScriptEntity& e, const vec3& p) -> Result<void> {
              auto tf = transform_of(*ctx, e);
              if (!tf.value) return std::unexpected(tf.value.error());
              glm::mat4 world = (*tf.value)->world;
              world[3] = glm::vec4(p, 1.0F);
              place(*ctx, ctx->scene->registry(), e.id, world);
              return {};
          }));
    t.set("translate", bind(L, [ctx](const ScriptEntity& e, const vec3& d) -> Result<void> {
              auto tf = transform_of(*ctx, e);
              if (!tf.value) return std::unexpected(tf.value.error());
              glm::mat4 world = (*tf.value)->world;
              world[3] += glm::vec4(d, 0.0F);
              place(*ctx, ctx->scene->registry(), e.id, world);
              return {};
          }));
    t.set("local_position", bind(L, [ctx](const ScriptEntity& e) -> Result<vec3> {
              auto tf = transform_of(*ctx, e);
              if (!tf.value) return std::unexpected(tf.value.error());
              return vec3((*tf.value)->local[3]);
          }));
    t.set("set_local_position", bind(L, [ctx](const ScriptEntity& e, const vec3& p) -> Result<void> {
              auto tf = transform_of(*ctx, e);
              if (!tf.value) return std::unexpected(tf.value.error());
              (*tf.value)->local[3] = glm::vec4(p, 1.0F);
              entt::registry& r = ctx->scene->registry();
              refresh_world(r, e.id);
              teleport_physics(*ctx, r, e.id);
              return {};
          }));
    t.set("rotation", bind(L, [ctx](const ScriptEntity& e) -> Result<quat> {
              auto tf = transform_of(*ctx, e);
              if (!tf.value) return std::unexpected(tf.value.error());
              return decompose((*tf.value)->world).rotation;
          }));
    t.set("set_rotation", bind(L, [ctx](const ScriptEntity& e, const quat& q) -> Result<void> {
              auto tf = transform_of(*ctx, e);
              if (!tf.value) return std::unexpected(tf.value.error());
              Trs trs = decompose((*tf.value)->world);
              trs.rotation = glm::normalize(q);
              place(*ctx, ctx->scene->registry(), e.id, compose(trs));
              return {};
          }));
    t.set("rotate", bind(L, [ctx](const ScriptEntity& e, const quat& q) -> Result<void> {
              auto tf = transform_of(*ctx, e);
              if (!tf.value) return std::unexpected(tf.value.error());
              Trs trs = decompose((*tf.value)->world);
              trs.rotation = glm::normalize(q * trs.rotation);
              place(*ctx, ctx->scene->registry(), e.id, compose(trs));
              return {};
          }));
    t.set("look_at", bind(L, [ctx](const ScriptEntity& e, const vec3& target,
                                   sol::optional<vec3> up) -> Result<void> {
              auto tf = transform_of(*ctx, e);
              if (!tf.value) return std::unexpected(tf.value.error());
              Trs trs = decompose((*tf.value)->world);
              const vec3 dir = target - trs.position;
              if (glm::length(dir) < 1e-6F) return {};
              const vec3 u = up.value_or(vec3(0.0F, 1.0F, 0.0F));
              const vec3 f = glm::normalize(dir);
              trs.rotation = glm::quatLookAtRH(
                  f, std::abs(glm::dot(f, glm::normalize(u))) > 0.9999F ? vec3(0.0F, 0.0F, 1.0F) : u);
              place(*ctx, ctx->scene->registry(), e.id, compose(trs));
              return {};
          }));
    t.set("scale", bind(L, [ctx](const ScriptEntity& e) -> Result<vec3> {
              auto tf = transform_of(*ctx, e);
              if (!tf.value) return std::unexpected(tf.value.error());
              return decompose((*tf.value)->local).scale;
          }));
    t.set("set_scale", bind(L, [ctx](const ScriptEntity& e, sol::object s) -> Result<void> {
              auto tf = transform_of(*ctx, e);
              if (!tf.value) return std::unexpected(tf.value.error());
              vec3 scale(1.0F);
              if (s.is<float>()) {
                  scale = vec3(s.as<float>());
              } else if (s.is<vec3>()) {
                  scale = s.as<vec3>();
              } else {
                  return fail("set_scale expects a number or a vec3");
              }
              Trs trs = decompose((*tf.value)->local);
              trs.scale = scale;
              (*tf.value)->local = compose(trs);
              refresh_world(ctx->scene->registry(), e.id);
              return {};
          }));
    const auto axis = [ctx](vec3 local_axis) {
        return [ctx, local_axis](const ScriptEntity& e) -> Result<vec3> {
            auto tf = transform_of(*ctx, e);
            if (!tf.value) return std::unexpected(tf.value.error());
            return decompose((*tf.value)->world).rotation * local_axis;
        };
    };
    t.set("forward", bind(L, axis(vec3(0.0F, 0.0F, -1.0F))));
    t.set("right", bind(L, axis(vec3(1.0F, 0.0F, 0.0F))));
    t.set("up", bind(L, axis(vec3(0.0F, 1.0F, 0.0F))));
}

void bind_components(sol::usertype<ScriptEntity>& t, lua_State* L, ApiContext& c) {
    ApiContext* ctx = &c;
    t.set("get", bind(L, [ctx](const ScriptEntity& e, const std::string& name,
                               sol::this_state s) -> Result<sol::object> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              if (scene::find_component_info(name) == nullptr) {
                  return fail("unknown component '" + name + "' (see scene.components in docs/scripting.md)");
              }
              const nlohmann::json all = scene::encode_components(**r.value, e.id);
              const auto it = all.find(name);
              if (it == all.end()) return sol::make_object(s.lua_state(), sol::lua_nil);
              return json_to_lua(s.lua_state(), *it);
          }));
    t.set("set", bind(L, [ctx](const ScriptEntity& e, const std::string& name,
                               const sol::object& fields) -> Result<void> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              auto json = lua_to_json(fields, true);
              if (!json.value) return fail(name + ": " + json.value.error().message);
              if (auto ok = scene::apply_component(**r.value, e.id, name, *json.value, true); !ok) {
                  return fail(ok.error().message);
              }
              if (name == "transform") {
                  refresh_world(**r.value, e.id);
                  teleport_physics(*ctx, **r.value, e.id);
              }
              return {};
          }));
    t.set("has", bind(L, [ctx](const ScriptEntity& e, const std::string& name) -> Result<bool> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              const auto names = scene::component_names(**r.value, e.id);
              return std::ranges::find(names, name) != names.end();
          }));
    t.set("remove", bind(L, [ctx](const ScriptEntity& e, const std::string& name) -> Result<bool> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              if (name == "transform") return fail("the transform cannot be removed");
              if (scene::find_component_info(name) == nullptr) return fail("unknown component '" + name + "'");
              return scene::remove_component(**r.value, e.id, name);
          }));
}

void bind_hierarchy(sol::usertype<ScriptEntity>& t, lua_State* L, ApiContext& c) {
    ApiContext* ctx = &c;
    t.set("name", bind(L, [ctx](const ScriptEntity& e) -> Result<std::string> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              const auto* n = (*r.value)->try_get<scene::Name>(e.id);
              return n != nullptr ? n->value : std::string();
          }));
    t.set("set_name", bind(L, [ctx](const ScriptEntity& e, const std::string& name) -> Result<void> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              if (name.size() > 256) return fail("names are at most 256 bytes");
              (*r.value)->emplace_or_replace<scene::Name>(e.id, name);
              return {};
          }));
    t.set("parent", bind(L, [ctx](const ScriptEntity& e, sol::this_state s) -> Result<sol::object> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              const auto* p = (*r.value)->try_get<scene::Parent>(e.id);
              if (p == nullptr || p->entity == entt::null || !(*r.value)->valid(p->entity)) {
                  return sol::make_object(s.lua_state(), sol::lua_nil);
              }
              return sol::make_object(s.lua_state(), ScriptEntity{p->entity});
          }));
    t.set("set_parent", bind(L, [ctx](const ScriptEntity& e, sol::optional<ScriptEntity> parent)
                                    -> Result<void> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              entt::entity p = entt::null;
              if (parent) {
                  auto pr = live_entity(*ctx, *parent);
                  if (!pr.value) return std::unexpected(pr.value.error());
                  p = parent->id;
              }
              const glm::mat4 world = (*r.value)->get<Transform>(e.id).world;
              if (!ctx->scene->set_parent(e.id, p)) {
                  return fail("set_parent refused: that would make the entity its own ancestor");
              }
              scene::set_world_transform(**r.value, e.id, world); // keep the world pose
              refresh_world(**r.value, e.id);
              return {};
          }));
    t.set("children", bind(L, [ctx](const ScriptEntity& e, sol::this_state s) -> Result<sol::object> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              sol::table out(s.lua_state(), sol::create);
              if (const auto* ch = (*r.value)->try_get<scene::Children>(e.id)) {
                  int i = 1;
                  for (const entt::entity child : ch->entities) {
                      if ((*r.value)->valid(child)) out[i++] = ScriptEntity{child};
                  }
              }
              return sol::object(out);
          }));
    t.set("destroy", bind(L, [ctx](const ScriptEntity& e) -> Result<void> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              ctx->pending_destroy.push_back(e.id);
              return {};
          }));
    t.set("script", bind(L, [ctx](const ScriptEntity& e, sol::this_state s) -> Result<sol::object> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              if (ctx->instance_of) return ctx->instance_of(e.id);
              return sol::make_object(s.lua_state(), sol::lua_nil);
          }));
}

void bind_physics(sol::usertype<ScriptEntity>& t, lua_State* L, ApiContext& c) {
    ApiContext* ctx = &c;
    t.set("add_impulse", bind(L, [ctx](const ScriptEntity& e, const vec3& impulse) -> Result<void> {
              auto body = dynamic_body(*ctx, e);
              if (!body.value) return std::unexpected(body.value.error());
              ctx->services.physics->add_impulse(*body.value, impulse);
              return {};
          }));
    t.set("set_velocity", bind(L, [ctx](const ScriptEntity& e, const vec3& v) -> Result<void> {
              auto body = dynamic_body(*ctx, e);
              if (!body.value) return std::unexpected(body.value.error());
              ctx->services.physics->set_linear_velocity(*body.value, v);
              return {};
          }));
    t.set("velocity", bind(L, [ctx](const ScriptEntity& e) -> Result<vec3> {
              auto r = live_entity(*ctx, e);
              if (!r.value) return std::unexpected(r.value.error());
              if (const auto* cc = (*r.value)->try_get<scene::CharacterController>(e.id)) {
                  return cc->velocity;
              }
              auto body = dynamic_body(*ctx, e);
              if (!body.value) return std::unexpected(body.value.error());
              return ctx->services.physics->linear_velocity(*body.value);
          }));
    t.set("move", bind(L, [ctx](const ScriptEntity& e, const vec3& dir) -> Result<void> {
              auto cc = component_of<scene::CharacterController>(*ctx, e, "character",
                                                                  "add a character component");
              if (!cc.value) return std::unexpected(cc.value.error());
              vec3 m(dir.x, 0.0F, dir.z);
              if (glm::length(m) > 1.0F) m = glm::normalize(m);
              (*cc.value)->move = m;
              return {};
          }));
    t.set("jump", bind(L, [ctx](const ScriptEntity& e, float speed) -> Result<bool> {
              auto cc = component_of<scene::CharacterController>(*ctx, e, "character",
                                                                  "add a character component");
              if (!cc.value) return std::unexpected(cc.value.error());
              if (!(*cc.value)->on_ground) return false;
              (*cc.value)->velocity.y = std::max(speed, 0.0F);
              (*cc.value)->on_ground = false;
              return true;
          }));
    t.set("on_ground", bind(L, [ctx](const ScriptEntity& e) -> Result<bool> {
              auto cc = component_of<scene::CharacterController>(*ctx, e, "character",
                                                                  "add a character component");
              if (!cc.value) return std::unexpected(cc.value.error());
              return (*cc.value)->on_ground;
          }));
    t.set("set_nav_target", bind(L, [ctx](const ScriptEntity& e, const vec3& target) -> Result<void> {
              auto a = component_of<scene::NavAgent>(*ctx, e, "nav_agent", "add a nav_agent component");
              if (!a.value) return std::unexpected(a.value.error());
              scene::nav_agent_command(**a.value, target);
              return {};
          }));
    t.set("stop_nav", bind(L, [ctx](const ScriptEntity& e) -> Result<void> {
              auto a = component_of<scene::NavAgent>(*ctx, e, "nav_agent", "add a nav_agent component");
              if (!a.value) return std::unexpected(a.value.error());
              scene::nav_agent_stop(**a.value);
              return {};
          }));
    t.set("nav_arrived", bind(L, [ctx](const ScriptEntity& e) -> Result<bool> {
              auto a = component_of<scene::NavAgent>(*ctx, e, "nav_agent", "add a nav_agent component");
              if (!a.value) return std::unexpected(a.value.error());
              return (*a.value)->arrived;
          }));
    const auto sound = [ctx](scene::AudioCommand command) {
        return [ctx, command](const ScriptEntity& e) -> Result<void> {
            auto a = component_of<scene::AudioSource>(*ctx, e, "audio_source",
                                                      "add an audio_source with a clip");
            if (!a.value) return std::unexpected(a.value.error());
            (*a.value)->command = command;
            return {};
        };
    };
    t.set("play_sound", bind(L, sound(scene::AudioCommand::play)));
    t.set("stop_sound", bind(L, sound(scene::AudioCommand::stop)));
}

} // namespace

void register_entity(LuaVm& vm, ApiContext& c) {
    sol::state_view lua = vm.lua();
    lua_State* L = vm.state();
    ApiContext* ctx = &c;
    sol::automagic_enrollments none;
    none.default_constructor = false;
    none.pairs_operator = false;
    none.to_string_operator = false;
    none.call_operator = false;
    none.less_than_operator = false;
    none.less_than_or_equal_to_operator = false;
    none.length_operator = false;
    auto t = lua.new_usertype<ScriptEntity>("Entity", none);
    t.set("id", sol::property([](const ScriptEntity& e) { return entity_id(e.id); }));
    t.set("valid", [ctx](const ScriptEntity& e) {
        return ctx->scene != nullptr && ctx->scene->registry().valid(e.id);
    });
    t.set(sol::meta_function::to_string, bind(L, [ctx](const ScriptEntity& e) -> Result<std::string> {
              if (ctx->scene == nullptr || !ctx->scene->registry().valid(e.id)) {
                  return "Entity(" + std::to_string(entity_id(e.id)) + ", destroyed)";
              }
              return "Entity(" + describe_entity(ctx->scene->registry(), e.id) + ")";
          }));
    bind_transform(t, L, c);
    bind_components(t, L, c);
    bind_hierarchy(t, L, c);
    bind_physics(t, L, c);
}

} // namespace engine::script::detail
