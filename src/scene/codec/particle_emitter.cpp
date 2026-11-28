// Codec component `particle_emitter`: GPU particle emitter (sparks, smoke, dust): emission, shape, forces, curves, flipbook, blending, collision.
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

std::expected<void, core::Error> register_particle_emitter(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "particle_emitter",
                         .summary = "GPU particle emitter (sparks, smoke, dust): emission, shape, forces, curves, flipbook, blending, collision",
                         .addable = true,
                         .writable = true,
                         .order = 2000};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        json out;
        const auto* p = &r.get<ParticleEmitter>(e);
        out["particle_emitter"] = detail::encode_particle_emitter(p->settings);
        return std::move(out["particle_emitter"]);
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "particle_emitter";
        return merge<ParticleEmitter>(r, e, v, name, strict, [](JsonFields& f, ParticleEmitter& p) {
            detail::decode_particle_emitter(f, p.settings);
        });
    };
    return registry.add(typed_codec<ParticleEmitter>(std::move(codec)));
}

} // namespace engine::scene::codec
