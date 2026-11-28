// Codec component `dir_light`: Directional light (the sun).
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

std::expected<void, core::Error> register_dir_light(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "dir_light",
                         .summary = "Directional light (the sun)",
                         .addable = true,
                         .writable = true,
                         .order = 700};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const DirectionalLight& l = r.get<DirectionalLight>(e);
        return json({{"direction", vec3_json(l.direction)},
                     {"color", vec3_json(l.color)},
                     {"intensity", l.intensity}});
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "dir_light";
        return merge<DirectionalLight>(r, e, v, name, strict, [](JsonFields& f, DirectionalLight& l) {
            f.vec3("direction", l.direction).color3("color", l.color).number("intensity", l.intensity, 0.0F);
            if (glm::length(l.direction) < 1e-6F) {
                f.fail("direction", "must be non-zero");
            } else {
                l.direction = glm::normalize(l.direction);
            }
        });
    };
    return registry.add(typed_codec<DirectionalLight>(std::move(codec)));
}

} // namespace engine::scene::codec
