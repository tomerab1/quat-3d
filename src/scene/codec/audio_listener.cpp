// Codec component `audio_listener`: The audio listener follows this entity (else the camera).
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

std::expected<void, core::Error> register_audio_listener(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "audio_listener",
                         .summary = "The audio listener follows this entity (else the camera)",
                         .addable = true,
                         .writable = true,
                         .order = 2400};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const AudioListener& l = r.get<AudioListener>(e);
        return json({{"active", l.active}});
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "audio_listener";
        return merge<AudioListener>(r, e, v, name, strict,
                                    [](JsonFields& f, AudioListener& l) { f.boolean("active", l.active); });
    };
    return registry.add(typed_codec<AudioListener>(std::move(codec)));
}

} // namespace engine::scene::codec
