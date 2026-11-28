// Codec component `character`: Capsule character controller.
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

} // namespace

std::expected<void, core::Error> register_character(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "character",
                         .summary = "Capsule character controller",
                         .addable = true,
                         .writable = true,
                         .order = 1100};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        json out;
        const auto* c = &r.get<CharacterController>(e);
        out["character"] = {{"radius", c->radius},
                            {"half_height", c->half_height},
                            {"speed", c->speed}};
        return std::move(out["character"]);
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "character";
        auto ok = merge<CharacterController>(r, e, v, name, strict,
                                         [](JsonFields& f, CharacterController& c) {
            f.number("radius", c.radius, 0.01F).number("half_height", c.half_height, 0.0F);
            f.number("speed", c.speed, 0.0F);
        });
        if (ok) invalidate_physics(r, e, name);
        return ok;
    };
    return registry.add(typed_codec<CharacterController>(std::move(codec)));
}

} // namespace engine::scene::codec
