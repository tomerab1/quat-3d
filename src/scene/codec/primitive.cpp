// Codec component `primitive`: Procedural shape (cube, sphere, plane, cylinder, cone, capsule) with a PBR colour or a material shader.
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

std::expected<void, core::Error> register_primitive(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "primitive",
                         .summary = "Procedural shape (cube, sphere, plane, cylinder, cone, capsule) with a PBR colour or a material shader",
                         .addable = true,
                         .writable = true,
                         .order = 200};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const Primitive& p = r.get<Primitive>(e);
        return json(primitive_fields_json(p));
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "primitive";
        auto ok = merge<Primitive>(r, e, v, name, strict, [](JsonFields& f, Primitive& p) {
            read_primitive_fields(f, p);
            p.resolved = 0; // rebuild the MeshRenderer
        });
        if (ok) r.remove<MeshSource>(e); // the primitive is now the mesh's provenance
        return ok;
    };
    codec.remove = [](entt::registry& r, entt::entity e) {
        const bool had = r.remove<Primitive>(e) > 0;
        if (had) r.remove<MeshRenderer>(e);
        return had;
    };
    return registry.add(typed_codec<Primitive>(std::move(codec)));
}

} // namespace engine::scene::codec
