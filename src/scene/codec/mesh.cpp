// Codec component `mesh`: glTF mesh provenance (read-only).
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

} // namespace

std::expected<void, core::Error> register_mesh(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "mesh",
                         .summary = "glTF mesh provenance (read-only)",
                         .addable = false,
                         .writable = false,
                         .order = 2900};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const MeshSource& m = r.get<MeshSource>(e);
        json out = {{"mesh_key", m.mesh_key}, {"material_key", m.material_key}, {"gltf", m.gltf_path}};
        if (m.gltf_node >= 0) out["node"] = m.gltf_node;
        return out;
    };
    codec.remove = [](entt::registry& r, entt::entity e) {
        const bool had = r.remove<MeshSource>(e) > 0;
        r.remove<MeshRenderer, SkinnedMesh, Animator>(e);
        return had;
    };
    return registry.add(typed_codec<MeshSource>(std::move(codec)));
}

} // namespace engine::scene::codec
