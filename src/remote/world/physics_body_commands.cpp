// Body control of a running simulation (ROADMAP A10.1): physics.get,
// physics.set and physics.apply. The contract is docs/remote-control.md
// ("Physics control"); the engine calls are PhysicsWorld's body-control API.

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "internal.hpp"

namespace engine::remote::world {

namespace {

using physics::PhysicsWorld;

constexpr float k_max_speed = 1.0e4F;         // m/s
constexpr float k_max_spin_deg = 1.0e5F;      // deg/s
constexpr float k_max_magnitude = 1.0e7F;     // N, N*s, N*m, N*m*s
constexpr float k_default_force_s = 1.0F / 60.0F;

constexpr core::EnumName<scene::BodyMotion> k_motions[] = {
    {"static", scene::BodyMotion::static_body},
    {"kinematic", scene::BodyMotion::kinematic},
    {"dynamic", scene::BodyMotion::dynamic}};

[[nodiscard]] const char* motion_name(physics::Motion m) {
    switch (m) {
    case physics::Motion::static_body:
        return "static";
    case physics::Motion::kinematic:
        return "kinematic";
    case physics::Motion::dynamic:
        return "dynamic";
    }
    return "static";
}

[[nodiscard]] std::string entity_label(entt::entity e) {
    return "entity " + std::to_string(entt::to_integral(e));
}

// The entity's live rigid body, or a precise error saying why there is none.
[[nodiscard]] std::expected<std::uint32_t, RpcError>
live_body(const entt::registry& r, const PhysicsWorld& world, entt::entity e) {
    const auto* rb = r.try_get<scene::RigidBody>(e);
    if (rb == nullptr) {
        if (r.all_of<scene::CharacterController>(e)) {
            return bad_params(entity_label(e) +
                              " is a character controller, not a rigid body: drive it with "
                              "character.move");
        }
        return bad_params(entity_label(e) +
                          " has no rigidbody; add collider + rigidbody components (scene.update) "
                          "and its body is built on the next simulated frame");
    }
    if (!world.body_state(rb->body)) {
        return rpc_fail(rpc_code::unavailable,
                        entity_label(e) +
                            "'s body is not built yet: bodies are (re)built on the next simulated "
                            "frame (step a paused simulation with debug.step)");
    }
    return rb->body;
}

[[nodiscard]] Json deg_json(const glm::vec3& radians) {
    return vec3_json(glm::degrees(radians));
}

[[nodiscard]] Json body_json(const entt::registry& r, entt::entity e,
                             const PhysicsWorld::BodyState& s) {
    Json out{{"entity", entity_json(e)}};
    if (const auto* n = r.try_get<scene::Name>(e)) out["name"] = n->value;
    out["kind"] = "body";
    out["motion_type"] = motion_name(s.motion);
    out["layer"] = std::string(layer_name(s.layer));
    out["sensor"] = s.sensor;
    out["sleeping"] = !s.awake;
    out["mass"] = s.mass;
    out["linear_velocity"] = vec3_json(s.linear_velocity);
    out["angular_velocity"] = deg_json(s.angular_velocity);
    out["speed"] = glm::length(s.linear_velocity);
    out["position"] = vec3_json(s.position);
    out["friction"] = s.friction;
    out["restitution"] = s.restitution;
    out["gravity_factor"] = s.gravity_factor;
    out["linear_damping"] = s.linear_damping;
    out["angular_damping"] = s.angular_damping;
    return out;
}

[[nodiscard]] Json character_json(const entt::registry& r, entt::entity e,
                                  const PhysicsWorld& world, const scene::CharacterController& cc) {
    Json out{{"entity", entity_json(e)}};
    if (const auto* n = r.try_get<scene::Name>(e)) out["name"] = n->value;
    out["kind"] = "character";
    out["position"] = vec3_json(world.character_position(cc.character));
    out["velocity"] = vec3_json(world.character_velocity(cc.character));
    out["on_ground"] = world.character_on_ground(cc.character);
    out["move"] = vec3_json(cc.move);
    out["speed"] = cc.speed;
    out["layer"] = "character";
    return out;
}

RpcResult physics_get(Context& ctx, const Json& params) {
    const entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());
    auto world = running_physics(ctx);
    if (!world) return std::unexpected(world.error());

    if (const auto* cc = r.try_get<scene::CharacterController>(*e);
        cc != nullptr && !r.all_of<scene::RigidBody>(*e)) {
        if (!(*world)->has_character(cc->character)) {
            return rpc_fail(rpc_code::unavailable,
                            entity_label(*e) + "'s character is built on the next simulated frame");
        }
        return character_json(r, *e, **world, *cc);
    }
    auto body = live_body(r, **world, *e);
    if (!body) return std::unexpected(body.error());
    return body_json(r, *e, *(*world)->body_state(*body));
}

// Reads an optional vector field bounded by `max` (magnitude).
void bounded_vec3(core::JsonFields& f, std::string_view key, std::optional<glm::vec3>& out,
                  float max, std::string_view unit) {
    if (!f.has(key)) {
        f.known({key});
        return;
    }
    glm::vec3 v{0.0F};
    f.vec3(key, v);
    if (!f.ok()) return;
    if (glm::length(v) > max) {
        f.fail(key, "magnitude " + std::to_string(glm::length(v)) + " exceeds " +
                        std::to_string(max) + " " + std::string(unit));
        return;
    }
    out = v;
}

RpcResult physics_set(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    std::optional<glm::vec3> linear;
    std::optional<glm::vec3> angular_deg;
    bounded_vec3(f, "linear_velocity", linear, k_max_speed, "m/s");
    bounded_vec3(f, "angular_velocity", angular_deg, k_max_spin_deg, "deg/s");
    std::optional<scene::BodyMotion> motion;
    if (f.has("motion_type")) {
        scene::BodyMotion m = scene::BodyMotion::dynamic;
        f.enumeration("motion_type", m, k_motions);
        motion = m;
    } else {
        f.known({"motion_type"});
    }
    const auto read_opt = [&](std::string_view key, float lo, float hi) -> std::optional<float> {
        if (!f.has(key)) {
            f.known({key});
            return std::nullopt;
        }
        float v = 0.0F;
        f.number(key, v, lo, hi);
        return v;
    };
    const auto gravity_factor = read_opt("gravity_factor", -10.0F, 10.0F);
    const auto friction = read_opt("friction", 0.0F, 10.0F);
    const auto restitution = read_opt("restitution", 0.0F, 1.0F);
    const auto linear_damping = read_opt("linear_damping", 0.0F, 100.0F);
    const auto angular_damping = read_opt("angular_damping", 0.0F, 100.0F);
    std::optional<bool> wake;
    if (f.has("wake")) {
        bool w = true;
        f.boolean("wake", w);
        wake = w;
    } else {
        f.known({"wake"});
    }
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());
    auto world_or = running_physics(ctx);
    if (!world_or) return std::unexpected(world_or.error());
    PhysicsWorld& world = **world_or;
    auto body_or = live_body(r, world, *e);
    if (!body_or) return std::unexpected(body_or.error());
    const std::uint32_t body = *body_or;
    auto& rb = r.get<scene::RigidBody>(*e);

    // Validate everything before changing anything.
    const scene::BodyMotion target = motion.value_or(rb.motion);
    bool rebuild = motion && *motion != rb.motion &&
                         (*motion == scene::BodyMotion::static_body ||
                          rb.motion == scene::BodyMotion::static_body);
    if ((linear || angular_deg) && target != scene::BodyMotion::dynamic) {
        return bad_params(std::string(linear ? "linear_velocity" : "angular_velocity") + ": " +
                          entity_label(*e) + "'s body is " +
                          (target == scene::BodyMotion::static_body ? "static" : "kinematic") +
                          "; only dynamic bodies take velocities (set motion_type \"dynamic\")");
    }
    if ((linear || angular_deg) && rebuild) {
        return bad_params("motion_type: switching to or from static rebuilds the body on the next "
                          "frame; set velocities in a later call");
    }
    if (wake.has_value() && !*wake && rebuild) {
        return bad_params("wake: the body is being rebuilt (motion_type change from/to static)");
    }

    if (motion && *motion != rb.motion) {
        // Dynamic <-> kinematic switches in place. When Jolt refuses (a body
        // built static whose component has since changed), the switch falls
        // back to a rebuild so the component and the body never disagree.
        if (!rebuild && !world.set_motion(body, *motion == scene::BodyMotion::dynamic
                                                     ? physics::Motion::dynamic
                                                     : physics::Motion::kinematic)) {
            if (linear || angular_deg || (wake.has_value() && !*wake)) {
                return rpc_fail(rpc_code::unavailable,
                                "motion_type: " + entity_label(*e) +
                                    "'s body cannot switch in place and is rebuilt on the next "
                                    "frame; set velocities or sleep in a later call");
            }
            rebuild = true;
        }
        rb.motion = *motion;
        if (rebuild) rb.body = PhysicsWorld::invalid_body; // physics_system sweeps and rebuilds it
    }
    // The component carries the authoring values (a rebuilt body keeps them).
    if (gravity_factor) rb.gravity_factor = *gravity_factor;
    if (friction) rb.friction = *friction;
    if (restitution) rb.restitution = *restitution;
    if (linear_damping) rb.linear_damping = *linear_damping;
    if (angular_damping) rb.angular_damping = *angular_damping;
    if (rebuild) {
        return Json{{"entity", entity_json(*e)},
                    {"rebuilt", true},
                    {"motion_type", motion_name(rb.motion == scene::BodyMotion::static_body
                                                    ? physics::Motion::static_body
                                                : rb.motion == scene::BodyMotion::kinematic
                                                    ? physics::Motion::kinematic
                                                    : physics::Motion::dynamic)}};
    }
    if (gravity_factor) world.set_gravity_factor(body, *gravity_factor);
    if (friction) world.set_friction(body, *friction);
    if (restitution) world.set_restitution(body, *restitution);
    if (linear_damping || angular_damping) {
        world.set_damping(body, rb.linear_damping, rb.angular_damping);
    }
    if (linear) world.set_linear_velocity(body, *linear);
    if (angular_deg) world.set_angular_velocity(body, glm::radians(*angular_deg));
    if (wake) world.set_awake(body, *wake);
    return body_json(r, *e, *world.body_state(body));
}

RpcResult physics_apply(Context& ctx, const Json& params) {
    entt::registry& r = ctx.scene->registry();
    core::JsonFields f(params, "");
    auto e = required_entity(f, r, "entity");
    std::optional<glm::vec3> impulse;
    std::optional<glm::vec3> force;
    std::optional<glm::vec3> angular_impulse;
    std::optional<glm::vec3> torque;
    std::optional<glm::vec3> at_point;
    bounded_vec3(f, "impulse", impulse, k_max_magnitude, "N*s");
    bounded_vec3(f, "force", force, k_max_magnitude, "N");
    bounded_vec3(f, "angular_impulse", angular_impulse, k_max_magnitude, "N*m*s");
    bounded_vec3(f, "torque", torque, k_max_magnitude, "N*m");
    bounded_vec3(f, "at_point", at_point, 1.0e6F, "m");
    float duration = k_default_force_s;
    const bool has_duration = f.has("duration_s");
    // Forces act in whole fixed steps: shorter than one step cannot be honoured.
    f.number("duration_s", duration, PhysicsWorld::fixed_timestep,
             PhysicsWorld::max_force_duration);
    if (auto ok = finish(f); !ok) return std::unexpected(ok.error());
    if (!e) return std::unexpected(e.error());

    const int kinds = (impulse ? 1 : 0) + (force ? 1 : 0) + (angular_impulse ? 1 : 0) +
                      (torque ? 1 : 0);
    if (kinds != 1) {
        return bad_params("give exactly one of impulse (N*s), force (N), angular_impulse (N*m*s) "
                          "or torque (N*m); got " +
                          std::to_string(kinds));
    }
    if (at_point && !impulse && !force) {
        return bad_params("at_point: only applies to impulse or force (angular ones act about the "
                          "centre of mass)");
    }
    if (has_duration && !force && !torque) {
        return bad_params("duration_s: only forces and torques act over time; impulses are "
                          "instantaneous");
    }
    auto world_or = running_physics(ctx);
    if (!world_or) return std::unexpected(world_or.error());
    PhysicsWorld& world = **world_or;
    auto body_or = live_body(r, world, *e);
    if (!body_or) return std::unexpected(body_or.error());
    const std::uint32_t body = *body_or;
    if (!world.is_dynamic(body)) {
        return bad_params(entity_label(*e) + "'s body is " +
                          motion_name(world.body_state(body)->motion) +
                          "; only dynamic bodies respond to impulses and forces "
                          "(physics.set motion_type \"dynamic\")");
    }

    std::string applied;
    if (impulse) {
        applied = "impulse";
        if (at_point) {
            world.add_impulse(body, *impulse, *at_point);
        } else {
            world.add_impulse(body, *impulse);
        }
    } else if (force) {
        applied = "force";
        if (at_point) {
            world.add_force(body, *force, *at_point, duration);
        } else {
            world.add_force(body, *force, duration);
        }
    } else if (angular_impulse) {
        applied = "angular_impulse";
        world.add_angular_impulse(body, *angular_impulse);
    } else {
        applied = "torque";
        world.add_torque(body, *torque, duration);
    }
    Json out = body_json(r, *e, *world.body_state(body));
    out["applied"] = applied;
    if (force || torque) {
        // What the world actually applies: the duration rounded to whole steps.
        out["duration_s"] = PhysicsWorld::force_duration(duration);
        out["steps"] = PhysicsWorld::force_steps(duration);
    }
    return out;
}

} // namespace

void register_physics_body_commands(RpcServer& server, const ContextPtr& ctx) {
    server.add_method("physics.get",
                      "A body's runtime state (play mode): {entity} -> {kind, motion_type, layer, "
                      "sleeping, mass, linear_velocity, angular_velocity (deg/s), position, "
                      "friction, restitution, gravity_factor, damping}.",
                      [ctx](const Json& p) { return physics_get(*ctx, p); });
    server.add_method("physics.set",
                      "Change a running body: {entity, linear_velocity?, angular_velocity? (deg/s), "
                      "motion_type?, gravity_factor?, friction?, restitution?, linear_damping?, "
                      "angular_damping?, wake?} -> the body's state.",
                      [ctx](const Json& p) { return physics_set(*ctx, p); });
    server.add_method("physics.apply",
                      "Push a dynamic body: {entity, impulse | force | angular_impulse | torque, "
                      "at_point?, duration_s?} -> the body's state.",
                      [ctx](const Json& p) { return physics_apply(*ctx, p); });
}

} // namespace engine::remote::world
