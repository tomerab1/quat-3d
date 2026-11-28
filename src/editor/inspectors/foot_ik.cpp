// Inspector section: Foot IK (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../animation_inspector.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::FootIK> {
    static constexpr const char* title = "Foot IK";
    static constexpr const char* codec = "foot_ik";
    static constexpr const char* icon = icon::footprints;
    static constexpr bool property_grid = true;
    static bool* enabled(scene::FootIK& k) { return &k.enabled; }
    static void draw(entt::registry& r, entt::entity e, scene::FootIK& foot_ik) {
        draw_foot_ik(r, e, foot_ik);
    }
};

void register_foot_ik_inspector(InspectorRegistry& registry) {
    registry.add({.name = "foot_ik",
                  .order = 1100,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::FootIK>(state, r, e, true);
        }});
}

} // namespace engine::editor
