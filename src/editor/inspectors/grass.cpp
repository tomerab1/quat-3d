// Inspector section: Grass (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../grass_inspector.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::Grass> {
    static constexpr const char* title = "Grass";
    static constexpr const char* codec = "grass";
    static constexpr const char* icon = icon::sprout;
    static void draw(entt::registry&, entt::entity, scene::Grass& grass) { draw_grass(grass); }
};

void register_grass_inspector(InspectorRegistry& registry) {
    registry.add({.name = "grass",
                  .order = 1300,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::Grass>(state, r, e, true);
        }});
}

} // namespace engine::editor
