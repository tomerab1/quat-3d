// Inspector section: Mesh Renderer (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include <cstdio>

#include "inspect.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::MeshRenderer> {
    static constexpr const char* title = "Mesh Renderer";
    static constexpr const char* icon = icon::box;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::MeshRenderer& mr) {
        char buf[128];
        if (mr.mesh.valid() && mr.mesh.is_loaded()) {
            std::snprintf(buf, sizeof(buf), "%u vertices, %u indices", mr.mesh->vertex_count,
                          mr.mesh->index_count);
            ui::prop_value("Mesh", buf, nullptr, true);
        } else {
            ui::prop_value("Mesh", "none");
        }
        if (mr.material.valid() && mr.material.is_loaded()) {
            const auto& p = mr.material->params;
            std::snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", static_cast<double>(p.base_color_factor.x),
                          static_cast<double>(p.base_color_factor.y),
                          static_cast<double>(p.base_color_factor.z));
            ui::prop_value("Base colour", buf, nullptr, true);
            std::snprintf(buf, sizeof(buf), "metal %.2f  rough %.2f",
                          static_cast<double>(p.metallic_factor),
                          static_cast<double>(p.roughness_factor));
            ui::prop_value("Surface", buf, nullptr, true);
            std::snprintf(buf, sizeof(buf), "0x%x", p.flags);
            ui::prop_value("Flags", buf, "Material feature flags", true);
        } else {
            ui::prop_value("Material", "default");
        }
    }
};

void register_mesh_renderer_inspector(InspectorRegistry& registry) {
    registry.add({.name = "mesh_renderer",
                  .order = 300,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            if (r.all_of<scene::Primitive>(e)) return; // the primitive section covers it
            inspect<scene::MeshRenderer>(state, r, e);
        }});
}

} // namespace engine::editor
