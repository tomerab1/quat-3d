// debug.watch, debug.raycast and debug.nav_path — ROADMAP A5 world queries
// for diagnosing a running game.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/sim_clock.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

constexpr std::size_t k_max_watch = 64;
constexpr float k_max_ray = 100'000.0F;

[[nodiscard]] std::string known_components() {
    std::string known;
    for (const auto& c : scene::component_infos()) {
        known += (known.empty() ? "" : ", ") + std::string(c.name);
    }
    return known;
}

// Runtime state of a nav agent that the codec does not expose (it is
// system-managed): why an NPC is or is not moving.
[[nodiscard]] Json nav_agent_state(const scene::NavAgent& a) {
    Json path = Json::array();
    for (const glm::vec3& p : a.path) path.push_back(vec3_json(p));
    return Json{{"active", a.active},
                {"paused", a.paused},
                {"arrived", a.arrived},
                {"failed", a.failed},
                {"commanded", a.commanded},
                {"plan_failures", a.plan_failures},
                {"path_partial", a.path_partial},
                {"waypoint", a.waypoint},
                {"path", std::move(path)},
                {"retry_s", a.retry_s},
                {"stuck_s", a.progress_timer}};
}

[[nodiscard]] Json watch_entity(const entt::registry& r, entt::entity e,
                                const std::vector<std::string>& only) {
    const Json all = scene::encode_components(r, e);
    Json components = Json::object();
    Json missing = Json::array();
    if (only.empty()) {
        components = all;
    } else {
        for (const std::string& name : only) {
            if (const auto it = all.find(name); it != all.end()) {
                components[name] = *it;
            } else {
                missing.push_back(name);
            }
        }
    }
    const auto* n = r.try_get<scene::Name>(e);
    Json out{{"id", entity_json(e)},
             {"name", n != nullptr ? n->value : std::string()},
             {"world_position", vec3_json(glm::vec3(world_matrix(r, e)[3]))},
             {"components", std::move(components)}};
    if (!missing.empty()) out["missing"] = std::move(missing);
    const bool want_nav =
        only.empty() || std::find(only.begin(), only.end(), "nav_agent") != only.end();
    if (const auto* agent = r.try_get<scene::NavAgent>(e); agent != nullptr && want_nav) {
        out["nav_agent_state"] = nav_agent_state(*agent);
    }
    return out;
}

RpcResult debug_watch(Context& ctx, const Json& params) {
    const entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    const Json* one = f.raw("entity");
    const Json* many = f.raw("entities");
    const Json* names = f.raw("components");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());

    std::vector<std::string> only;
    if (names != nullptr && !names->is_null()) {
        if (!names->is_array() || names->size() > 64) {
            return bad_params("components: expected an array of component names (e.g. "
                              "[\"transform\", \"rigidbody\"])");
        }
        for (const Json& n : *names) {
            if (!n.is_string() || scene::find_component_info(n.get_ref<const std::string&>()) ==
                                      nullptr) {
                return bad_params("components: unknown component " +
                                  n.dump(-1, ' ', false, Json::error_handler_t::replace) +
                                  " (known: " + known_components() + ")");
            }
            only.push_back(n.get<std::string>());
        }
    }

    const bool has_one = one != nullptr && !one->is_null();
    const bool has_many = many != nullptr && !many->is_null();
    if (has_one == has_many) {
        return bad_params("entity: give exactly one of `entity` (id or unique name) or "
                          "`entities` (array of up to 64)");
    }
    std::vector<entt::entity> targets;
    if (has_one) {
        auto e = resolve_entity(r, *one, "entity");
        if (!e) return std::unexpected(e.error());
        targets.push_back(*e);
    } else {
        if (!many->is_array() || many->empty() || many->size() > k_max_watch) {
            return bad_params("entities: expected an array of 1 to 64 entity ids or names");
        }
        for (std::size_t i = 0; i < many->size(); ++i) {
            auto e = resolve_entity(r, (*many)[i], "entities[" + std::to_string(i) + "]");
            if (!e) return std::unexpected(e.error());
            targets.push_back(*e);
        }
    }

    Json entities = Json::array();
    for (const entt::entity e : targets) entities.push_back(watch_entity(r, e, only));
    Json out{{"entities", std::move(entities)}};
    if (ctx.services.frame_stats) out["frame"] = ctx.services.frame_stats().frame;
    if (ctx.services.sim_clock != nullptr) {
        out["sim_time_s"] = ctx.services.sim_clock->state().sim_time_s;
    }
    return out;
}

RpcResult debug_raycast(Context& ctx, const Json& params) {
    const entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    glm::vec3 origin{0.0F};
    glm::vec3 direction{0.0F};
    float max_distance = 1000.0F;
    bool include_sensors = false;
    const bool has_origin = f.has("origin");
    const bool has_direction = f.has("direction");
    f.vec3("origin", origin)
        .vec3("direction", direction)
        .number("max_distance", max_distance, 1e-3F, k_max_ray)
        .boolean("include_sensors", include_sensors);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!has_origin) return bad_params("origin: required (world-space [x, y, z])");
    if (!has_direction || !(glm::length(direction) > 1e-6F)) {
        return bad_params("direction: required, a non-zero [x, y, z] (need not be normalized)");
    }
    const physics::PhysicsWorld* world =
        ctx.services.physics_world ? ctx.services.physics_world() : nullptr;
    if (world == nullptr) {
        return rpc_fail(rpc_code::unavailable,
                        "no physics world is running: physics bodies exist only in play mode "
                        "(sim.play) or the physics demo modes");
    }
    const auto hit = world->cast_ray(origin, direction, max_distance, include_sensors);
    if (!hit) return Json{{"hit", false}};

    Json entity = nullptr;
    std::string name;
    if (hit->owner != physics::PhysicsWorld::no_owner) {
        const auto e = static_cast<entt::entity>(hit->owner);
        if (r.valid(e)) {
            entity = entity_json(e);
            if (const auto* n = r.try_get<scene::Name>(e)) name = n->value;
        }
    }
    Json out{{"hit", true},
             {"entity", std::move(entity)},
             {"name", name},
             {"point", vec3_json(hit->point)},
             {"normal", vec3_json(hit->normal)},
             {"distance", hit->distance},
             {"sensor", hit->sensor}};
    if (hit->character != physics::PhysicsWorld::invalid_body) {
        out["kind"] = "character";
    } else {
        out["kind"] = hit->owner == physics::PhysicsWorld::no_owner ? "static_world" : "body";
    }
    return out;
}

RpcResult debug_nav_path(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    glm::vec3 from{0.0F};
    glm::vec3 to{0.0F};
    const bool has_from = f.has("from");
    const bool has_to = f.has("to");
    f.vec3("from", from).vec3("to", to);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!has_from) return bad_params("from: required (world-space [x, y, z])");
    if (!has_to) return bad_params("to: required (world-space [x, y, z])");
    const nav::NavMesh* mesh = ctx.services.navmesh ? ctx.services.navmesh() : nullptr;
    if (mesh == nullptr || !mesh->valid()) {
        return rpc_fail(rpc_code::unavailable, "no navmesh is baked; call nav.bake first");
    }
    auto path = mesh->plan_path(from, to);
    if (!path) return rpc_fail(rpc_code::failed, "no path: " + path.error().message);
    Json points = Json::array();
    float length = 0.0F;
    for (std::size_t i = 0; i < path->points.size(); ++i) {
        points.push_back(vec3_json(path->points[i]));
        if (i > 0) length += glm::distance(path->points[i - 1], path->points[i]);
    }
    return Json{{"points", std::move(points)}, {"partial", path->partial}, {"length", length}};
}

} // namespace

void register_debug_query_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("debug.watch",
                      "Snapshot of entities' components now: {entity | entities:[...], "
                      "components?:[names]} -> {entities:[{id, name, world_position, components, "
                      "nav_agent_state?}], frame, sim_time_s}.",
                      [ctx](const Json& p) { return debug_watch(*ctx, p); });
    server.add_method("debug.raycast",
                      "Physics raycast (play mode): {origin, direction, max_distance?=1000, "
                      "include_sensors?=false} -> {hit, entity, name, kind, point, normal, "
                      "distance}.",
                      [ctx](const Json& p) { return debug_raycast(*ctx, p); });
    server.add_method("debug.nav_path",
                      "Navmesh path query: {from, to} -> {points, partial, length}.",
                      [ctx](const Json& p) { return debug_nav_path(*ctx, p); });
}

} // namespace engine::remote::world
