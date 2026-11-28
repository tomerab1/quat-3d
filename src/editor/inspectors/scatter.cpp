// Inspector section: Scatter (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../inspector_world.hpp"
#include "engine/scene/scatter.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::Scatter> {
    static constexpr const char* title = "Scatter";
    static constexpr const char* codec = "scatter";
    static constexpr const char* icon = icon::trees;
    static constexpr bool property_grid = true;
    static void draw(entt::registry& r, entt::entity e, scene::Scatter& s) {
        draw_scatter(r, e, s);
    }
};

void register_scatter_inspector(InspectorRegistry& registry) {
    registry.add({.name = "scatter",
                  .order = 1700,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::Scatter>(state, r, e, true);
        }});
}

} // namespace engine::editor
