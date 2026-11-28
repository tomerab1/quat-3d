// Inspector section: Instanced Mesh (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../inspector_world.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::InstancedMesh> {
    static constexpr const char* title = "Instanced Mesh";
    static constexpr const char* codec = "instanced_mesh";
    static constexpr const char* icon = icon::boxes;
    static constexpr bool property_grid = true;
    static void draw(entt::registry& r, entt::entity e, scene::InstancedMesh& mesh,
                     const panels::InspectorState& state) {
        draw_instanced_mesh(r, e, mesh, state.project_root);
    }
};

void register_instanced_mesh_inspector(InspectorRegistry& registry) {
    registry.add({.name = "instanced_mesh",
                  .order = 1600,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::InstancedMesh>(state, r, e, true);
        }});
}

} // namespace engine::editor
