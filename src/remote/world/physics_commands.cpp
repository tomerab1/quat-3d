// Gameplay control of a running simulation (ROADMAP A10.1): events.poll,
// physics.get_gravity / set_gravity, character.move, and the physics helpers
// shared by the physics_*_commands.cpp files.
// The contract is docs/remote-control.md ("Physics control").

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/event_ring.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/physics_settings.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

// ---- Shared physics helpers (internal.hpp) --------------------------------------

std::expected<physics::PhysicsWorld*, RpcError> running_physics(const Context& ctx) {
    physics::PhysicsWorld* w = ctx.services.physics_control ? ctx.services.physics_control()
                                                            : nullptr;
    if (w == nullptr) {
        return rpc_fail(rpc_code::unavailable,
                        "no physics world is running: bodies exist only in play mode (call "
                        "sim.play) or the physics demo modes");
    }
    return w;
}

namespace {

constexpr std::array<std::pair<std::string_view, physics::Layer>, 5> k_layers = {{
    {"static", physics::Layer::static_body},
    {"dynamic", physics::Layer::dynamic},
    {"character", physics::Layer::character},
    {"trigger", physics::Layer::trigger},
    {"sensor", physics::Layer::sensor},
}};

} // namespace

std::string_view layer_name(physics::Layer layer) {
    for (const auto& [name, value] : k_layers) {
        if (value == layer) return name;
    }
    return "unknown";
}

std::expected<void, RpcError> read_layer_mask(const Json* layers, std::uint32_t& mask) {
    if (layers == nullptr || layers->is_null()) return {};
    constexpr const char* expected =
        "layers: expected a non-empty array of layer names: static, dynamic, character, "
        "trigger, sensor";
    if (!layers->is_array() || layers->empty() || layers->size() > k_layers.size()) {
        return bad_params(expected);
    }
    std::uint32_t out = 0;
    for (const Json& l : *layers) {
        bool matched = false;
        for (const auto& [name, value] : k_layers) {
            if (l.is_string() && l.get_ref<const std::string&>() == name) {
                out |= physics::layer_bit(value);
                matched = true;
            }
        }
        if (!matched) {
            return bad_params(std::string(expected) + " (got " +
                              l.dump(-1, ' ', false, Json::error_handler_t::replace) + ")");
        }
    }
    mask = out;
    return {};
}

void put_owner(const entt::registry& registry, std::uint32_t owner, Json& out) {
    if (owner == physics::PhysicsWorld::no_owner) {
        out["entity"] = nullptr;
        return;
    }
    out["entity"] = owner;
    const auto e = static_cast<entt::entity>(owner);
    if (registry.valid(e)) {
        if (const auto* n = registry.try_get<scene::Name>(e)) out["name"] = n->value;
    }
}

namespace {

constexpr int k_max_poll = 2000;

constexpr std::array<core::EventKind, 4> k_event_kinds = {
    core::EventKind::contact_added, core::EventKind::contact_removed,
    core::EventKind::trigger_enter, core::EventKind::trigger_exit};

[[nodiscard]] Json event_entity(std::uint32_t id) {
    if (id == core::Event::no_entity) return nullptr;
    return id;
}

[[nodiscard]] Json event_json(const entt::registry& r, const core::Event& e) {
    Json out{{"seq", e.seq},
             {"kind", std::string(core::to_string(e.kind))},
             {"time_s", e.time_s},
             {"entity_a", event_entity(e.entity_a)},
             {"entity_b", event_entity(e.entity_b)}};
    // Names help a reader without another lookup; entities destroyed since
    // keep their id only.
    for (const auto& [key, id] : {std::pair{"name_a", e.entity_a}, std::pair{"name_b", e.entity_b}}) {
        const auto ent = static_cast<entt::entity>(id);
        if (id == core::Event::no_entity || !r.valid(ent)) continue;
        if (const auto* n = r.try_get<scene::Name>(ent)) out[key] = n->value;
    }
    const bool has_point =
        e.kind == core::EventKind::contact_added || e.kind == core::EventKind::trigger_enter;
    if (has_point) out["point"] = Json::array({e.point[0], e.point[1], e.point[2]});
    if (e.kind == core::EventKind::contact_added) {
        out["normal"] = Json::array({e.normal[0], e.normal[1], e.normal[2]});
        out["impulse"] = e.impulse;
    }
    return out;
}

RpcResult events_poll(Context& ctx, const Json& params) {
    const entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    std::int64_t since = 0;
    int max = 200;
    f.integer("since", since, 0, std::numeric_limits<std::int64_t>::max())
        .integer("max", max, 1, k_max_poll);
    const Json* kinds = f.raw("kinds");
    const Json* entity_ref = f.raw("entity");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (ctx.services.event_ring == nullptr) return unavailable("the engine event ring");

    std::array<bool, k_event_kinds.size()> want{};
    want.fill(true);
    if (kinds != nullptr && !kinds->is_null()) {
        std::string known;
        for (const core::EventKind k : k_event_kinds) {
            known += (known.empty() ? "" : ", ") + std::string(core::to_string(k));
        }
        if (!kinds->is_array() || kinds->empty() || kinds->size() > k_event_kinds.size()) {
            return bad_params("kinds: expected a non-empty array of event kinds (" + known + ")");
        }
        want.fill(false);
        for (const Json& k : *kinds) {
            bool matched = false;
            for (std::size_t i = 0; i < k_event_kinds.size(); ++i) {
                if (k.is_string() && k.get_ref<const std::string&>() ==
                                         core::to_string(k_event_kinds[i])) {
                    want[i] = matched = true;
                }
            }
            if (!matched) {
                return bad_params("kinds: unknown kind " +
                                  k.dump(-1, ' ', false, Json::error_handler_t::replace) +
                                  " (known: " + known + ")");
            }
        }
    }
    std::uint32_t only = core::Event::no_entity;
    if (entity_ref != nullptr && !entity_ref->is_null()) {
        auto e = resolve_entity(r, *entity_ref, "entity");
        if (!e) return std::unexpected(e.error());
        only = static_cast<std::uint32_t>(entt::to_integral(*e));
    }

    // Read the whole retained window and filter here, so `max` counts the
    // matching events and the cursor stops right after the last one returned.
    const core::EventRing& ring = *ctx.services.event_ring;
    const auto since_seq = static_cast<std::uint64_t>(since);
    const core::EventQueryResult q = ring.query(since_seq, ring.capacity());
    Json events = Json::array();
    std::uint64_t cursor = q.cursor;
    bool more = false;
    for (const core::Event& e : q.events) {
        const bool kind_ok = want[static_cast<std::size_t>(e.kind)];
        const bool entity_ok =
            only == core::Event::no_entity || e.entity_a == only || e.entity_b == only;
        if (!kind_ok || !entity_ok) continue;
        if (events.size() == static_cast<std::size_t>(max)) {
            more = true; // stop after the last one returned
            break;
        }
        events.push_back(event_json(r, e));
        cursor = e.seq;
    }
    if (!more) cursor = q.cursor; // everything retained was examined
    // Sequence numbers in (since, cursor] that can no longer be read.
    std::uint64_t readable = 0;
    for (const core::Event& e : q.events) {
        if (e.seq > cursor) break;
        ++readable;
    }
    const std::uint64_t base = since_seq > q.last_seq ? 0 : since_seq;
    return Json{{"events", std::move(events)},
                {"cursor", cursor},
                {"more", more},
                {"dropped", cursor - base - readable},
                {"oldest_seq", q.oldest_seq},
                {"last_seq", q.last_seq},
                {"capacity", ring.capacity()}};
}

[[nodiscard]] Json gravity_json(const glm::vec3& g) {
    return Json{{"gravity", vec3_json(g)}, {"magnitude", glm::length(g)}};
}

RpcResult physics_get_gravity(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    return gravity_json(scene::physics_settings(ctx.scene->registry()).gravity);
}

// The scene's one gravity setting: saved with the scene, reverted by Stop
// (play snapshot), used by Jolt and the character controllers.
RpcResult physics_set_gravity(Context& ctx, const Json& params) {
    core::JsonFields f(params, "");
    const bool has = f.has("gravity");
    scene::PhysicsSettings next = scene::physics_settings(ctx.scene->registry());
    f.vec3("gravity", next.gravity);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!has) return bad_params("gravity: required, [x, y, z] in m/s^2 (Earth: [0, -9.81, 0])");
    if (glm::length(next.gravity) > scene::k_max_gravity) {
        return bad_params("gravity: magnitude " + std::to_string(glm::length(next.gravity)) +
                          " exceeds " + std::to_string(static_cast<int>(scene::k_max_gravity)) +
                          " m/s^2");
    }
    scene::physics_settings_mut(ctx.scene->registry()) = next;
    // A running world takes it now (the physics systems also sync every tick).
    if (physics::PhysicsWorld* w = ctx.services.physics_control ? ctx.services.physics_control()
                                                                : nullptr) {
        w->set_gravity(next.gravity);
    }
    return gravity_json(next.gravity);
}

constexpr float k_default_jump = 5.0F; // m/s
constexpr float k_max_jump = 50.0F;

// Drives a character controller like a player: a walk velocity or a target
// point, an optional jump, an optional time limit. character_system does
// the steering (steer_character) on the following ticks.
RpcResult character_move(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    std::optional<glm::vec3> velocity;
    std::optional<glm::vec3> target;
    if (f.has("velocity")) {
        glm::vec3 v{0.0F};
        f.vec3("velocity", v);
        velocity = v;
    }
    if (f.has("target")) {
        glm::vec3 t{0.0F};
        f.vec3("target", t);
        if (glm::any(glm::greaterThan(glm::abs(t), glm::vec3(1.0e6F)))) {
            f.fail("target", "coordinates must be within +-1e6 metres");
        }
        target = t;
    }
    f.known({"velocity", "target", "jump"});
    float jump = 0.0F;
    if (const Json* j = f.raw("jump"); j != nullptr && !j->is_null()) {
        if (bool b = false; core::JsonFields::read_bool(*j, b)) {
            jump = b ? k_default_jump : 0.0F;
        } else if (!core::JsonFields::read_float(*j, jump) || !(jump > 0.0F) || jump > k_max_jump) {
            f.fail("jump", "expected true (5 m/s) or a take-off speed in (0, 50] m/s");
        }
    }
    float duration = -1.0F;
    const bool has_duration = f.has("duration_s");
    f.number("duration_s", duration, 0.01F, 60.0F);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());
    if (velocity && target) {
        return bad_params("give `velocity` or `target`, not both (a target walks at the "
                          "controller's speed)");
    }
    if (!velocity && !target && jump <= 0.0F) {
        return bad_params("nothing to do: give `velocity`, `target` and/or `jump`");
    }
    if (has_duration && !velocity && !target) {
        return bad_params("duration_s: only limits a walk (`velocity` or `target`)");
    }
    const std::string label = "entity " + std::to_string(entt::to_integral(*e));
    auto* cc = r.try_get<scene::CharacterController>(*e);
    if (cc == nullptr) {
        return bad_params(label + " has no character controller; add a `character` component "
                                  "(scene.update)" +
                          (r.all_of<scene::RigidBody>(*e) ? " or push its body with physics.apply"
                                                          : ""));
    }
    if (const auto* agent = r.try_get<scene::NavAgent>(*e); agent != nullptr && agent->active) {
        return bad_params(label + " has an active nav_agent that steers it; set nav_agent.target "
                                  "instead, or stop it (scene.update nav_agent {active: false})");
    }
    if (r.all_of<scene::NetDriven>(*e)) {
        return bad_params(label + " is driven by the network (a remote player's inputs)");
    }
    auto world_or = running_physics(ctx);
    if (!world_or) return std::unexpected(world_or.error());
    physics::PhysicsWorld& world = **world_or;

    if (velocity) {
        const glm::vec2 horizontal(velocity->x, velocity->z);
        const float speed = glm::length(horizontal);
        if (speed > cc->speed + 1e-4F) {
            return bad_params("velocity: horizontal speed " + std::to_string(speed) +
                              " m/s exceeds the controller's speed " + std::to_string(cc->speed) +
                              " (raise character.speed with scene.update)");
        }
        cc->move = speed > 0.0F && cc->speed > 0.0F
                       ? glm::vec3(horizontal.x, 0.0F, horizontal.y) / cc->speed
                       : glm::vec3(0.0F);
        cc->has_move_target = false;
    }
    if (target) {
        cc->move_target = *target;
        cc->has_move_target = true;
    }
    if (velocity || target) cc->move_time_left = has_duration ? duration : -1.0F;
    if (jump > 0.0F) cc->jump_speed = jump;

    Json out{{"entity", entity_json(*e)}, {"move", vec3_json(cc->move)}};
    if (world.has_character(cc->character)) {
        out["position"] = vec3_json(world.character_position(cc->character));
        out["velocity"] = vec3_json(world.character_velocity(cc->character));
        out["on_ground"] = world.character_on_ground(cc->character);
    } else {
        out["position"] = vec3_json(glm::vec3(world_matrix(r, *e)[3]));
        out["on_ground"] = false; // its controller is built on the next tick
    }
    if (cc->has_move_target) out["target"] = vec3_json(cc->move_target);
    return out;
}

} // namespace

void register_physics_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("character.move",
                      "Drive a character controller: {entity, velocity? | target?, jump?, "
                      "duration_s?} -> {position, velocity, on_ground, move, target?}.",
                      [ctx](const Json& p) { return character_move(*ctx, p); });
    server.add_method("physics.get_gravity", "The scene's gravity: {} -> {gravity, magnitude}.",
                      [ctx](const Json& p) { return physics_get_gravity(*ctx, p); });
    server.add_method("physics.set_gravity",
                      "Set the scene's gravity (Jolt + characters, saved with the scene): "
                      "{gravity:[x,y,z]} -> {gravity, magnitude}.",
                      [ctx](const Json& p) { return physics_set_gravity(*ctx, p); });
    server.add_method("events.poll",
                      "Gameplay events after a cursor: {since?=0, max?=200, kinds?:[contact_added|"
                      "contact_removed|trigger_enter|trigger_exit], entity?} -> {events:[{seq, kind, "
                      "time_s, entity_a, entity_b, point?, normal?, impulse?}], cursor, more, "
                      "dropped, oldest_seq, last_seq}.",
                      [ctx](const Json& p) { return events_poll(*ctx, p); });
}

} // namespace engine::remote::world
