// Inspector section: Camera (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../tuning_panels.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::Camera> {
    static constexpr const char* title = "Camera";
    static constexpr const char* codec = "camera";
    static constexpr const char* icon = icon::camera;
    static constexpr bool property_grid = true;
    static void draw(entt::registry& r, entt::entity e, scene::Camera& camera) {
        (void)ui::prop_angle("Field of view", camera.fov_y,
                             {.min = 10.0F, .max = 140.0F, .format = "%.0f°", .slider = true},
                             {.tooltip = "Vertical field of view"});
        (void)ui::prop_float("Near", camera.near_z, {.speed = 0.01F, .min = 0.001F, .max = 100.0F,
                                                     .format = "%.3f m"});
        (void)ui::prop_float("Far", camera.far_z, {.speed = 1.0F, .min = 0.1F, .max = 100000.0F,
                                                   .format = "%.0f m"});
        // Exactly one camera owns the play view: activating this one
        // deactivates the rest (radio semantics, like a "main camera" tag).
        if (ui::prop_bool("Active", camera.is_active,
                          {.tooltip = "Play + Game camera views through the active camera"}) &&
            camera.is_active) {
            for (auto [other, cam] : r.view<scene::Camera>().each()) {
                if (other != e) cam.is_active = false;
            }
        }
        ui::prop_note("Put the camera on a child entity offset from the body; on the mesh itself "
                      "the view sits inside the geometry.");
        // R20 physical lens: exposure (with exposure "physical"), depth of
        // field and the motion blur shutter.
        ui::prop_separator("Lens");
        (void)draw_lens_controls(camera.lens, camera.fov_y);
    }
};

void register_camera_inspector(InspectorRegistry& registry) {
    registry.add({.name = "camera",
                  .order = 700,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::Camera>(state, r, e, true);
        }});
}

} // namespace engine::editor
