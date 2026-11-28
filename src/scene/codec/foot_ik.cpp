// Codec component `foot_ik`: Foot placement on uneven ground for an animated character (leg IK + pelvis).
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

} // namespace

std::expected<void, core::Error> register_foot_ik(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "foot_ik",
                         .summary = "Foot placement on uneven ground for an animated character (leg IK + pelvis)",
                         .addable = true,
                         .writable = true,
                         .order = 2600};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const FootIK& k = r.get<FootIK>(e);
        return json(detail::encode_foot_ik(k));
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "foot_ik";
        return merge<FootIK>(r, e, v, name, strict, detail::decode_foot_ik);
    };
    return registry.add(typed_codec<FootIK>(std::move(codec)));
}

} // namespace engine::scene::codec
