// Inspector section: Rigid Body (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../physics_inspector.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::RigidBody> {
    static constexpr const char* title = "Rigid Body";
    static constexpr const char* codec = "rigidbody";
    static constexpr const char* icon = icon::atom;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::RigidBody& body) {
        draw_rigid_body_fields(body);
    }
};

void register_rigidbody_inspector(InspectorRegistry& registry) {
    registry.add({.name = "rigidbody",
                  .order = 2300,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::RigidBody>(state, r, e, true);
        }});
}

} // namespace engine::editor
