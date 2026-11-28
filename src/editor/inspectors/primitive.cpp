// Inspector section: Primitive (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../inspector_world.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::Primitive> {
    static constexpr const char* title = "Primitive";
    static constexpr const char* codec = "primitive";
    static constexpr const char* icon = icon::shapes;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::Primitive& p) {
        draw_primitive(p);
    }
};

void register_primitive_inspector(InspectorRegistry& registry) {
    registry.add({.name = "primitive",
                  .order = 200,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::Primitive>(state, r, e, true);
        }});
}

} // namespace engine::editor
