// Codec component `collider`: Physics collision shape (box, sphere, capsule).
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

constexpr EnumName<ColliderShape> k_collider_shapes[] = {
    {"box", ColliderShape::box}, {"sphere", ColliderShape::sphere},
    {"capsule", ColliderShape::capsule}};

} // namespace

std::expected<void, core::Error> register_collider(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "collider",
                         .summary = "Physics collision shape (box, sphere, capsule)",
                         .addable = true,
                         .writable = true,
                         .order = 900};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        json out;
        const auto* c = &r.get<Collider>(e);
        out["collider"] = {{"shape", enum_name(k_collider_shapes, c->shape)},
                           {"half_extents", vec3_json(c->half_extents)},
                           {"offset", vec3_json(c->offset)},
                           {"sensor", c->is_sensor}};
        return std::move(out["collider"]);
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "collider";
        auto ok = merge<Collider>(r, e, v, name, strict, [](JsonFields& f, Collider& c) {
            f.enumeration("shape", c.shape, k_collider_shapes);
            f.vec3("half_extents", c.half_extents).vec3("offset", c.offset);
            f.boolean("sensor", c.is_sensor);
            if (glm::any(glm::lessThanEqual(c.half_extents, glm::vec3(0.0F)))) {
                f.fail("half_extents", "components must be > 0");
            }
        });
        if (ok) invalidate_physics(r, e, name);
        return ok;
    };
    return registry.add(typed_codec<Collider>(std::move(codec)));
}

} // namespace engine::scene::codec
