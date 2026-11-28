// Codec component `material`: Per-entity overrides of the drawn material's PBR factors (null clears one).
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/material_override.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::EnumName;
using core::JsonFields;

} // namespace

std::expected<void, core::Error> register_material(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "material",
                         .summary = "Per-entity overrides of the drawn material's PBR factors (null clears one)",
                         .addable = true,
                         .writable = true,
                         .order = 300};
    codec.has = [](const entt::registry& r, entt::entity e) {
        const auto* m = r.try_get<MaterialOverride>(e);
        return m != nullptr && m->mask != 0;
    };
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        json out;
        const auto* m = &r.get<MaterialOverride>(e);
        out["material"] = encode_material_override(*m);
        return std::move(out["material"]);
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        MaterialOverride o = r.all_of<MaterialOverride>(e) ? r.get<MaterialOverride>(e)
                                                           : MaterialOverride{};
        if (auto ok = decode_material_override(v, o, strict); !ok) return ok;
        if (o.mask == 0) {
            r.remove<MaterialOverride>(e);
        } else {
            r.emplace_or_replace<MaterialOverride>(e, std::move(o));
        }
        return {};
    };
    return registry.add(typed_codec<MaterialOverride>(std::move(codec)));
}

} // namespace engine::scene::codec
