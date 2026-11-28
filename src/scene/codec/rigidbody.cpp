// Codec component `rigidbody`: Physics body: static, kinematic or dynamic.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::EnumName;
using core::JsonFields;

constexpr EnumName<BodyMotion> k_motions[] = {{"static", BodyMotion::static_body},
                                              {"kinematic", BodyMotion::kinematic},
                                              {"dynamic", BodyMotion::dynamic}};

} // namespace

std::expected<void, core::Error> register_rigidbody(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "rigidbody",
                         .summary = "Physics body: static, kinematic or dynamic",
                         .addable = true,
                         .writable = true,
                         .order = 1000};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const RigidBody& b = r.get<RigidBody>(e);
        return json({{"motion", enum_name(k_motions, b.motion)},
                     {"mass", b.mass},
                     {"friction", b.friction},
                     {"restitution", b.restitution},
                     {"gravity_factor", b.gravity_factor},
                     {"linear_damping", b.linear_damping},
                     {"angular_damping", b.angular_damping}});
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "rigidbody";
        auto ok = merge<RigidBody>(r, e, v, name, strict, [](JsonFields& f, RigidBody& b) {
            f.enumeration("motion", b.motion, k_motions).number("mass", b.mass, 0.0F);
            f.number("friction", b.friction, 0.0F, 10.0F);
            f.number("restitution", b.restitution, 0.0F, 1.0F);
            f.number("gravity_factor", b.gravity_factor, -10.0F, 10.0F);
            f.number("linear_damping", b.linear_damping, 0.0F, 100.0F);
            f.number("angular_damping", b.angular_damping, 0.0F, 100.0F);
        });
        if (ok) invalidate_physics(r, e, name);
        return ok;
    };
    return registry.add(typed_codec<RigidBody>(std::move(codec)));
}

} // namespace engine::scene::codec
