// Codec component `world_clock`: The scene's date, time and place (one per scene): drives the sun, moon and sky.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/world_clock.hpp"
#include "engine/scene/world_clock_json.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;

} // namespace

std::expected<void, core::Error> register_world_clock(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "world_clock",
                         .summary = "The scene's date, time and place (one per scene): drives the sun, moon and sky",
                         .addable = true,
                         .writable = true,
                         .order = 2100};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const WorldClock& c = r.get<WorldClock>(e);
        return json(encode_world_clock(c));
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "world_clock";
        // One clock per scene: a second entity cannot take the component.
        if (const entt::entity owner = find_world_clock(r); owner != entt::null && owner != e) {
            return std::unexpected(core::Error{
                "world_clock: the scene already has its clock on entity " +
                std::to_string(entt::to_integral(owner)) + " (edit that one, or remove it first)"});
        }
        return merge<WorldClock>(r, e, v, name, strict, decode_world_clock);
    };
    return registry.add(typed_codec<WorldClock>(std::move(codec)));
}

} // namespace engine::scene::codec
