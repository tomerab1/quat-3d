// Inspector section: Terrain (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../inspector_world.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::Terrain> {
    static constexpr const char* title = "Terrain";
    static constexpr const char* codec = "terrain";
    static constexpr const char* icon = icon::mountain;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::Terrain& terrain) {
        draw_terrain(terrain);
    }
};

void register_terrain_inspector(InspectorRegistry& registry) {
    registry.add({.name = "terrain",
                  .order = 1200,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::Terrain>(state, r, e, true);
        }});
}

} // namespace engine::editor
