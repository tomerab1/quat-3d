// Codec component `animator`: Animation playback of a glTF clip: clip switching (inertialized or crossfaded), time, speed, looping.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "scene/component_json_animation.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::EnumName;
using core::JsonFields;

} // namespace

std::expected<void, core::Error> register_animator(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "animator",
                         .summary = "Animation playback of a glTF clip: clip switching (inertialized or crossfaded), time, speed, looping",
                         .addable = false,
                         .writable = true,
                         .order = 2500};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        json out;
        const auto* a = &r.get<Animator>(e);
        out["animator"] = detail::encode_animator(*a);
        return std::move(out["animator"]);
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "animator";
        if (!r.all_of<Animator>(e)) {
            return std::unexpected(core::Error{
                "animator: the entity has no animation (animators come from glTF files)"});
        }
        return merge<Animator>(r, e, v, name, strict, detail::decode_animator);
    };
    return registry.add(typed_codec<Animator>(std::move(codec)));
}

} // namespace engine::scene::codec
