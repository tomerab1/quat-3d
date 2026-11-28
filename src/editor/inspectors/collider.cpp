// Inspector section: Collider (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../physics_inspector.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::Collider> {
    static constexpr const char* title = "Collider";
    static constexpr const char* codec = "collider";
    static constexpr const char* icon = icon::shield;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::Collider& collider) {
        draw_collider_fields(collider);
    }
};

void register_collider_inspector(InspectorRegistry& registry) {
    registry.add({.name = "collider",
                  .order = 2200,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::Collider>(state, r, e, true);
        }});
}

} // namespace engine::editor
