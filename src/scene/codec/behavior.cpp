// Codec component `behavior`: Behaviour tree (JSON source).
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::JsonFields;

} // namespace

std::expected<void, core::Error> register_behavior(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "behavior",
                         .summary = "Behaviour tree (JSON source)",
                         .addable = true,
                         .writable = true,
                         .order = 1400};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const BehaviorTree& b = r.get<BehaviorTree>(e);
        return json({{"source", b.source}, {"enabled", b.enabled}});
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "behavior";
        return merge<BehaviorTree>(r, e, v, name, strict, [](JsonFields& f, BehaviorTree& b) {
            if (const json* src = f.raw("source")) {
                if (src->is_string()) {
                    b.source = src->get<std::string>();
                } else if (src->is_object()) { // a tree given inline as JSON
                    b.source = src->dump(-1, ' ', false, json::error_handler_t::replace);
                } else {
                    f.fail("source", "expected a behaviour tree (object) or its JSON text");
                }
                b.compiled.reset(); // recompile lazily
                b.node_state.clear();
                b.last_result.clear();
                b.parse_failed = false;
            }
            f.boolean("enabled", b.enabled);
        });
    };
    return registry.add(typed_codec<BehaviorTree>(std::move(codec)));
}

} // namespace engine::scene::codec
