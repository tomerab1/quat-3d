// Codec component `joint`: Physics joint to another body or the world: fixed, point, hinge, slider, distance.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::EnumName;
using core::JsonFields;

constexpr EnumName<JointType> k_joint_types[] = {
    {"fixed", JointType::fixed},   {"point", JointType::point},
    {"hinge", JointType::hinge},   {"slider", JointType::slider},
    {"distance", JointType::distance}};
constexpr EnumName<JointMotorMode> k_motor_modes[] = {{"off", JointMotorMode::off},
                                                      {"velocity", JointMotorMode::velocity},
                                                      {"position", JointMotorMode::position}};


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

std::expected<void, core::Error> register_joint(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "joint",
                         .summary = "Physics joint to another body or the world: fixed, point, hinge, slider, distance",
                         .addable = true,
                         .writable = true,
                         .order = 1200};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const Joint& j = r.get<Joint>(e);
        return json(encode_joint(j));
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "joint";
        auto ok = merge<Joint>(r, e, v, name, strict, [&](JsonFields& f, Joint& j) {
            f.enumeration("type", j.type, k_joint_types);
            read_target(f, r, e, j);
            f.vec3("anchor", j.anchor).vec3("target_anchor", j.target_anchor).vec3("axis", j.axis);
            read_limits(f, j);
            read_motor(f, j);
            (void)f.raw("active"); // read-only (encoded for readers)
            check_joint(f, j);
        });
        if (ok) invalidate_physics(r, e, name);
        return ok;
    };
    return registry.add(typed_codec<Joint>(std::move(codec)));
}

} // namespace engine::scene::codec
