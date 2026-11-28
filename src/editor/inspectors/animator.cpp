// Inspector section: Animator (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../animation_inspector.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::Animator> {
    static constexpr const char* title = "Animator";
    static constexpr const char* codec = "animator";
    static constexpr const char* icon = icon::clapperboard;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::Animator& animator) {
        draw_animator(animator);
    }
};

void register_animator_inspector(InspectorRegistry& registry) {
    registry.add({.name = "animator",
                  .order = 1000,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::Animator>(state, r, e);
        }});
}

} // namespace engine::editor
