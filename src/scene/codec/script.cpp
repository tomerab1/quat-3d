// Codec component `script`: Lua gameplay script (assets/scripts/) with property overrides.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/script_component.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::JsonFields;

} // namespace

std::expected<void, core::Error> register_script(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "script",
                         .summary = "Lua gameplay script (assets/scripts/) with property overrides",
                         .addable = true,
                         .writable = true,
                         .order = 1600};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const Script& sc = r.get<Script>(e);
        return json(encode_script(sc));
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "script";
        // A remote request must name the file (the model gets a precise
        // error). Lenient decoding — the editor's Add Component, which adds
        // every component from {}, and scene files — accepts a script without
        // one yet: an empty path runs nothing until a file is picked.
        const bool adding = !r.all_of<Script>(e);
        return merge<Script>(r, e, v, name, strict, [adding, strict](JsonFields& f, Script& sc) {
            if (strict && adding && !f.has("path")) {
                f.fail("path", "required when adding a script (relative to assets/scripts/)");
            }
            decode_script_fields(f, sc);
        });
    };
    return registry.add(typed_codec<Script>(std::move(codec)));
}

} // namespace engine::scene::codec
