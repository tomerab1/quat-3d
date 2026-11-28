// Codec component `point_light`: Point light at the entity position.
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
using core::JsonFields;

} // namespace

std::expected<void, core::Error> register_point_light(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "point_light",
                         .summary = "Point light at the entity position",
                         .addable = true,
                         .writable = true,
                         .order = 600};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const PointLight& l = r.get<PointLight>(e);
        return json({{"color", vec3_json(l.color)},
                     {"radius", l.radius},
                     {"intensity", l.intensity}});
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "point_light";
        return merge<PointLight>(r, e, v, name, strict, [](JsonFields& f, PointLight& l) {
            f.color3("color", l.color).number("radius", l.radius, 0.01F).number("intensity", l.intensity, 0.0F);
        });
    };
    return registry.add(typed_codec<PointLight>(std::move(codec)));
}

} // namespace engine::scene::codec
