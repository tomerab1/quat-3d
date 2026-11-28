// Physics components of the component JSON codec: collider, rigidbody,
// character and joint (A10.1). See component_json_physics.hpp and
// include/engine/scene/component_json.hpp.

#include "component_json_physics.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"

namespace engine::scene::detail {

namespace {

using json = nlohmann::json;
using core::EnumName;
using core::JsonFields;

constexpr EnumName<ColliderShape> k_collider_shapes[] = {
    {"box", ColliderShape::box}, {"sphere", ColliderShape::sphere},
    {"capsule", ColliderShape::capsule}};
constexpr EnumName<BodyMotion> k_motions[] = {{"static", BodyMotion::static_body},
                                              {"kinematic", BodyMotion::kinematic},
                                              {"dynamic", BodyMotion::dynamic}};
constexpr EnumName<JointType> k_joint_types[] = {
    {"fixed", JointType::fixed},   {"point", JointType::point},
    {"hinge", JointType::hinge},   {"slider", JointType::slider},
    {"distance", JointType::distance}};
constexpr EnumName<JointMotorMode> k_motor_modes[] = {{"off", JointMotorMode::off},
                                                      {"velocity", JointMotorMode::velocity},
                                                      {"position", JointMotorMode::position}};

template <typename E, std::size_t N>
[[nodiscard]] std::string enum_name(const EnumName<E> (&names)[N], E value) {
    for (const auto& n : names) {
        if (n.value == value) return std::string(n.name);
    }
    return "unknown";
}

[[nodiscard]] json vec3_json(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

template <typename T, typename Decode>
[[nodiscard]] std::expected<void, core::Error> merge(entt::registry& r, entt::entity e,
                                                     const json& v, std::string_view name,
                                                     bool strict, Decode&& decode) {
    T component = r.all_of<T>(e) ? r.get<T>(e) : T{};
    JsonFields f(v, std::string(name), strict);
    decode(f, component);
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
    r.emplace_or_replace<T>(e, std::move(component));
    return {};
}

// Editing a physics component invalidates its body / character (or, for a
// joint, only its constraint): resetting the handle makes the physics
// systems' orphan sweep drop the old one and build a new one from the
// edited data on the next tick. A rebuilt body takes its joints with it;
// joint_system rebuilds them.
void invalidate_physics(entt::registry& r, entt::entity e, std::string_view edited) {
    if (edited == "joint") {
        if (auto* j = r.try_get<Joint>(e)) j->constraint = physics::PhysicsWorld::invalid_body;
        return;
    }
    if (auto* rb = r.try_get<RigidBody>(e)) rb->body = physics::PhysicsWorld::invalid_body;
    if (auto* cc = r.try_get<CharacterController>(e)) {
        cc->character = physics::PhysicsWorld::invalid_body;
    }
}

// ---- Joint ------------------------------------------------------------------

[[nodiscard]] json encode_joint(const Joint& j) {
    json out{{"type", enum_name(k_joint_types, j.type)},
             {"target",
              j.target == entt::null ? json(nullptr) : json(entt::to_integral(j.target))},
             {"anchor", vec3_json(j.anchor)},
             {"axis", vec3_json(j.axis)},
             {"limits", j.limits_enabled ? json::array({j.limits.x, j.limits.y}) : json(nullptr)},
             {"active", j.constraint != physics::PhysicsWorld::invalid_body}};
    if (j.type == JointType::distance) out["target_anchor"] = vec3_json(j.target_anchor);
    if (j.motor == JointMotorMode::off) {
        out["motor"] = nullptr;
    } else {
        out["motor"] = {{"mode", enum_name(k_motor_modes, j.motor)},
                        {"target", j.motor_target},
                        {"max_force", j.motor_max_force}};
    }
    return out;
}

// `target`: an entity id, a unique entity name, or null (the world).
void read_target(JsonFields& f, const entt::registry& r, entt::entity self, Joint& j) {
    f.known({"target"});
    if (!f.has("target")) return;
    const json* v = f.raw("target");
    if (v->is_null()) {
        j.target = entt::null;
        return;
    }
    if (v->is_string()) {
        const std::string& name = v->get_ref<const std::string&>();
        entt::entity found = entt::null;
        int matches = 0;
        for (auto [e, n] : r.view<const Name>().each()) {
            if (n.value == name) {
                found = e;
                ++matches;
            }
        }
        if (matches != 1) {
            f.fail("target", matches == 0 ? "no entity named '" + name + "'"
                                          : "'" + name + "' names " + std::to_string(matches) +
                                                " entities; use an id");
            return;
        }
        j.target = found;
    } else {
        std::int64_t id = 0;
        if (!JsonFields::read_int(*v, id) || id < 0 ||
            id > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
            f.fail("target", "expected an entity id, a unique entity name, or null (the world)");
            return;
        }
        const auto e = static_cast<entt::entity>(static_cast<std::uint32_t>(id));
        if (!r.valid(e)) {
            f.fail("target", "no entity with id " + std::to_string(id));
            return;
        }
        j.target = e;
    }
    if (j.target == self) f.fail("target", "a joint cannot target its own entity");
}

void read_limits(JsonFields& f, Joint& j) {
    f.known({"limits"});
    if (!f.has("limits")) return;
    const json* v = f.raw("limits");
    if (v->is_null()) {
        j.limits_enabled = false;
        return;
    }
    glm::vec2 l{};
    if (!v->is_array() || v->size() != 2 || !JsonFields::read_float((*v)[0], l.x) ||
        !JsonFields::read_float((*v)[1], l.y)) {
        f.fail("limits", "expected [min, max] or null (no limits)");
        return;
    }
    j.limits_enabled = true;
    j.limits = l;
}

void read_motor(JsonFields& f, Joint& j) {
    f.known({"motor"});
    if (!f.has("motor")) return;
    const json* v = f.raw("motor");
    if (v->is_null()) {
        j.motor = JointMotorMode::off;
        return;
    }
    JsonFields m(*v, "joint.motor", /*strict=*/true);
    m.enumeration("mode", j.motor, k_motor_modes);
    m.number("target", j.motor_target, -1.0e5F, 1.0e5F);
    m.number("max_force", j.motor_max_force, 0.0F, 1.0e9F);
    if (auto ok = m.finish(); !ok) f.fail("motor", ok.error().message);
}

// Per-type rules (what Jolt accepts, stated for the author).
void check_joint(JsonFields& f, const Joint& j) {
    if (!(glm::length(j.axis) > 1e-6F)) f.fail("axis", "must be a non-zero [x, y, z]");
    const bool takes_motor = j.type == JointType::hinge || j.type == JointType::slider;
    if (j.motor != JointMotorMode::off && !takes_motor) {
        f.fail("motor", "only hinge and slider joints have a motor");
    }
    if (!j.limits_enabled) return;
    const float lo = j.limits.x;
    const float hi = j.limits.y;
    switch (j.type) {
    case JointType::fixed:
    case JointType::point:
        f.fail("limits", "fixed and point joints have no limits (use null)");
        break;
    case JointType::hinge:
        if (!(lo >= -180.0F && lo <= 0.0F && hi >= 0.0F && hi <= 180.0F && lo < hi)) {
            f.fail("limits", "hinge limits are [min, max] degrees with min in [-180, 0], max in "
                             "[0, 180] and min < max (the angle is measured from the pose the "
                             "joint is built in)");
        }
        break;
    case JointType::slider:
        if (!(lo <= 0.0F && hi >= 0.0F && lo < hi && lo >= -1.0e4F && hi <= 1.0e4F)) {
            f.fail("limits", "slider limits are [min, max] metres with min <= 0 <= max and "
                             "min < max (measured from the pose the joint is built in)");
        }
        break;
    case JointType::distance:
        if (!(lo >= 0.0F && hi > 0.0F && lo <= hi && hi <= 1.0e5F)) {
            f.fail("limits", "distance limits are [min, max] metres with 0 <= min <= max, max > 0");
        }
        break;
    }
}

} // namespace

bool is_physics_component(std::string_view name) {
    for (const std::string_view n : k_physics_components) {
        if (n == name) return true;
    }
    return false;
}

bool has_physics_component(const entt::registry& r, entt::entity e, std::string_view name) {
    if (name == "collider") return r.all_of<Collider>(e);
    if (name == "rigidbody") return r.all_of<RigidBody>(e);
    if (name == "character") return r.all_of<CharacterController>(e);
    if (name == "joint") return r.all_of<Joint>(e);
    return false;
}

void encode_physics_component(const entt::registry& r, entt::entity e, std::string_view name,
                              json& out) {
    if (name == "collider") {
        if (const auto* c = r.try_get<Collider>(e)) {
            out["collider"] = {{"shape", enum_name(k_collider_shapes, c->shape)},
                               {"half_extents", vec3_json(c->half_extents)},
                               {"offset", vec3_json(c->offset)},
                               {"sensor", c->is_sensor}};
        }
    } else if (name == "rigidbody") {
        if (const auto* b = r.try_get<RigidBody>(e)) {
            out["rigidbody"] = {{"motion", enum_name(k_motions, b->motion)},
                                {"mass", b->mass},
                                {"friction", b->friction},
                                {"restitution", b->restitution},
                                {"gravity_factor", b->gravity_factor},
                                {"linear_damping", b->linear_damping},
                                {"angular_damping", b->angular_damping}};
        }
    } else if (name == "character") {
        if (const auto* c = r.try_get<CharacterController>(e)) {
            out["character"] = {{"radius", c->radius},
                                {"half_height", c->half_height},
                                {"speed", c->speed}};
        }
    } else if (name == "joint") {
        if (const auto* j = r.try_get<Joint>(e)) out["joint"] = encode_joint(*j);
    }
}

std::expected<void, core::Error> apply_physics_component(entt::registry& r, entt::entity e,
                                                         std::string_view name, const json& v,
                                                         bool strict) {
    std::expected<void, core::Error> ok;
    if (name == "collider") {
        ok = merge<Collider>(r, e, v, name, strict, [](JsonFields& f, Collider& c) {
            f.enumeration("shape", c.shape, k_collider_shapes);
            f.vec3("half_extents", c.half_extents).vec3("offset", c.offset);
            f.boolean("sensor", c.is_sensor);
            if (glm::any(glm::lessThanEqual(c.half_extents, glm::vec3(0.0F)))) {
                f.fail("half_extents", "components must be > 0");
            }
        });
    } else if (name == "rigidbody") {
        ok = merge<RigidBody>(r, e, v, name, strict, [](JsonFields& f, RigidBody& b) {
            f.enumeration("motion", b.motion, k_motions).number("mass", b.mass, 0.0F);
            f.number("friction", b.friction, 0.0F, 10.0F);
            f.number("restitution", b.restitution, 0.0F, 1.0F);
            f.number("gravity_factor", b.gravity_factor, -10.0F, 10.0F);
            f.number("linear_damping", b.linear_damping, 0.0F, 100.0F);
            f.number("angular_damping", b.angular_damping, 0.0F, 100.0F);
        });
    } else if (name == "character") {
        ok = merge<CharacterController>(r, e, v, name, strict,
                                        [](JsonFields& f, CharacterController& c) {
            f.number("radius", c.radius, 0.01F).number("half_height", c.half_height, 0.0F);
            f.number("speed", c.speed, 0.0F);
        });
    } else if (name == "joint") {
        ok = merge<Joint>(r, e, v, name, strict, [&](JsonFields& f, Joint& j) {
            f.enumeration("type", j.type, k_joint_types);
            read_target(f, r, e, j);
            f.vec3("anchor", j.anchor).vec3("target_anchor", j.target_anchor).vec3("axis", j.axis);
            read_limits(f, j);
            read_motor(f, j);
            (void)f.raw("active"); // read-only (encoded for readers)
            check_joint(f, j);
        });
    } else {
        return std::unexpected(
            core::Error{"component '" + std::string(name) + "' is not a physics component"});
    }
    if (ok) invalidate_physics(r, e, name);
    return ok;
}

bool remove_physics_component(entt::registry& r, entt::entity e, std::string_view name) {
    if (name == "collider") return r.remove<Collider>(e) > 0;
    if (name == "rigidbody") return r.remove<RigidBody>(e) > 0;
    if (name == "character") return r.remove<CharacterController>(e) > 0;
    if (name == "joint") return r.remove<Joint>(e) > 0;
    return false;
}

} // namespace engine::scene::detail
