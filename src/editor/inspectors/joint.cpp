// Inspector section: Joint (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../physics_inspector.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::Joint> {
    static constexpr const char* title = "Joint";
    static constexpr const char* codec = "joint";
    static constexpr const char* icon = icon::link;
    static constexpr bool property_grid = true;
    static void draw(entt::registry& r, entt::entity, scene::Joint& joint) {
        draw_joint_fields(r, joint);
    }
};

void register_joint_inspector(InspectorRegistry& registry) {
    registry.add({.name = "joint",
                  .order = 2400,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::Joint>(state, r, e, true);
        }});
}

} // namespace engine::editor
