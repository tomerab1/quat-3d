// Inspector section: Camera Rig Input (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../camera_rig_inspector.hpp"
#include "engine/scene/camera_rig.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::CameraRigInput> {
    static constexpr const char* title = "Camera Rig Input";
    static constexpr const char* codec = "camera_rig_input";
    static constexpr const char* icon = icon::orbit;
    static constexpr bool property_grid = true;
    static void draw(entt::registry&, entt::entity, scene::CameraRigInput& in) { draw_camera_rig_input(in); }
};

void register_camera_rig_input_inspector(InspectorRegistry& registry) {
    registry.add({.name = "camera_rig_input",
                  .order = 900,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::CameraRigInput>(state, r, e, true);
        }});
}

} // namespace engine::editor
