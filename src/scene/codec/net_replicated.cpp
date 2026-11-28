// Codec component `net_replicated`: Replicate this entity's transform to network clients.
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

std::expected<void, core::Error> register_net_replicated(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "net_replicated",
                         .summary = "Replicate this entity's transform to network clients",
                         .addable = true,
                         .writable = true,
                         .order = 2200};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const NetReplicated& n = r.get<NetReplicated>(e);
        return json({{"net_id", n.net_id}});
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "net_replicated";
        return merge<NetReplicated>(r, e, v, name, strict, [](JsonFields& f, NetReplicated& n) {
            f.integer("net_id", n.net_id, 0, 0xFFFFFFFFLL);
        });
    };
    return registry.add(typed_codec<NetReplicated>(std::move(codec)));
}

} // namespace engine::scene::codec
