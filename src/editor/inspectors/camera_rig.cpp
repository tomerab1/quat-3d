// Inspector section: Camera Rig (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../camera_rig_inspector.hpp"
#include "engine/scene/camera_rig.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::CameraRig> {
    static constexpr const char* title = "Camera Rig";
    static constexpr const char* codec = "camera_rig";
    static constexpr const char* icon = icon::orbit;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::CameraRig& rig) { draw_camera_rig(rig); }
};

void register_camera_rig_inspector(InspectorRegistry& registry) {
    registry.add({.name = "camera_rig",
                  .order = 800,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::CameraRig>(state, r, e, true);
        }});
}

} // namespace engine::editor
