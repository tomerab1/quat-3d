// Inspector editors for camera_rig / camera_rig_input — see
// camera_rig_inspector.hpp.

#include "camera_rig_inspector.hpp"

#include <array>
#include <cmath>
#include <cstdio>

#include "engine/scene/camera_rig.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace {

constexpr std::array k_bound_shapes = {
    ui::EnumItem{static_cast<int>(scene::RigBounds::Shape::none), "None", "The focus may go anywhere"},
    ui::EnumItem{static_cast<int>(scene::RigBounds::Shape::rect), "Rectangle", "An XZ rectangle"},
    ui::EnumItem{static_cast<int>(scene::RigBounds::Shape::circle), "Circle", "An XZ circle"},
};

constexpr ui::DragSpec k_deg{.speed = 0.5F, .min = -89.0F, .max = 89.0F, .format = "%.1f°"};
constexpr ui::DragSpec k_halflife{.speed = 0.005F, .min = 0.0F, .max = 10.0F, .format = "%.3f s"};
constexpr ui::DragSpec k_metres{.speed = 0.1F, .min = 0.01F, .max = 100000.0F, .format = "%.1f m"};

} // namespace

void draw_camera_rig(scene::CameraRig& rig) {
    (void)ui::prop_vec3("Focus", rig.focus, {.speed = 0.1F, .format = "%.2f"},
                        {.tooltip = "The ground point the camera orbits (target)"});
    (void)ui::prop_float("Yaw", rig.yaw_deg, {.speed = 0.5F, .format = "%.1f°"},
                         {.tooltip = "Heading: 0 looks along +X, 90 along +Z"});
    (void)ui::prop_float("Distance", rig.distance, k_metres);
    (void)ui::prop_float("Min distance", rig.min_distance, k_metres);
    (void)ui::prop_float("Max distance", rig.max_distance, k_metres);
    (void)ui::prop_bool("Pitch follows zoom", rig.pitch_follows_zoom,
                        {.tooltip = "Ease pitch between the near and far values on log distance"});
    if (rig.pitch_follows_zoom) {
        (void)ui::prop_float("Pitch near", rig.pitch_near_deg, k_deg, {.tooltip = "Pitch at the minimum distance"});
        (void)ui::prop_float("Pitch far", rig.pitch_far_deg, k_deg, {.tooltip = "Pitch at the maximum distance"});
    } else {
        (void)ui::prop_float("Pitch", rig.pitch_deg, k_deg, {.tooltip = "Negative looks down"});
    }
    (void)ui::prop_float("Yaw step", rig.yaw_step_deg, {.speed = 1.0F, .min = 0.0F, .max = 180.0F, .format = "%.0f°"},
                         {.tooltip = "0 rotates freely; 45 or 90 snaps"});

    ui::prop_separator("Feel");
    (void)ui::prop_float("Focus half-life", rig.focus_halflife_s, k_halflife);
    (void)ui::prop_float("Yaw half-life", rig.yaw_halflife_s, k_halflife);
    (void)ui::prop_float("Zoom half-life", rig.zoom_halflife_s, k_halflife);
    (void)ui::prop_float("Pitch half-life", rig.pitch_halflife_s, k_halflife);
    (void)ui::prop_float("Drag inertia", rig.drag_inertia_halflife_s, k_halflife,
                         {.tooltip = "Half-life of the release velocity after a drag (0: none)"});

    ui::prop_separator("Ground");
    (void)ui::prop_bool("Follow ground", rig.follow_ground, {.tooltip = "The focus height follows the terrain"});
    (void)ui::prop_float("Ground smoothing", rig.focus_ground_halflife_s, k_halflife);
    (void)ui::prop_float("Clearance", rig.ground_clearance, {.speed = 0.05F, .min = 0.0F, .max = 1000.0F, .format = "%.2f m"},
                         {.tooltip = "The camera stays this far above the ground (by looking down more)"});

    ui::prop_separator("Bounds");
    (void)ui::prop_enum("Shape", rig.bounds.shape, k_bound_shapes);
    if (rig.bounds.shape == scene::RigBounds::Shape::rect) {
        (void)ui::prop_vec2("Min (XZ)", rig.bounds.min, {.speed = 0.5F, .format = "%.1f"});
        (void)ui::prop_vec2("Max (XZ)", rig.bounds.max, {.speed = 0.5F, .format = "%.1f"});
    } else if (rig.bounds.shape == scene::RigBounds::Shape::circle) {
        (void)ui::prop_vec2("Centre (XZ)", rig.bounds.center, {.speed = 0.5F, .format = "%.1f"});
        (void)ui::prop_float("Radius", rig.bounds.radius, {.speed = 0.5F, .min = 0.0F, .max = 1.0e6F, .format = "%.1f m"});
    }
    if (rig.bounds.shape != scene::RigBounds::Shape::none) {
        (void)ui::prop_float("Margin", rig.bounds.margin, {.speed = 0.1F, .min = 0.0F, .max = 1.0e4F, .format = "%.1f m"},
                             {.tooltip = "How far a drag may pull the focus out before it springs back"});
    }

    if (rig.state.valid) {
        ui::prop_separator("Now");
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.1f %.1f %.1f", static_cast<double>(rig.state.focus.x),
                      static_cast<double>(rig.state.focus.y), static_cast<double>(rig.state.focus.z));
        ui::prop_value("Focus", buf, "The damped focus", true);
        std::snprintf(buf, sizeof(buf), "yaw %.1f°  pitch %.1f°  %.1f m", static_cast<double>(rig.state.yaw_deg),
                      static_cast<double>(rig.state.pitch_deg), static_cast<double>(std::exp(rig.state.log_distance)));
        ui::prop_value("Pose", buf, nullptr, true);
    }
}

void draw_camera_rig_input(scene::CameraRigInput& in) {
    (void)ui::prop_text("Pan", in.pan, {.tooltip = "axis2d action, rate: x right, y forward"}, true);
    (void)ui::prop_text("Orbit", in.orbit, {.tooltip = "axis1d action, rate: positive turns right"}, true);
    (void)ui::prop_text("Orbit delta", in.orbit_delta, {.tooltip = "axis1d action, per frame (mouse delta)"}, true);
    (void)ui::prop_text("Pitch delta", in.pitch_delta, {.tooltip = "axis1d action, per frame; free pitch only"}, true);
    (void)ui::prop_text("Zoom", in.zoom, {.tooltip = "axis1d action, wheel steps; positive zooms in"}, true);
    (void)ui::prop_text("Drag pan", in.drag_pan, {.tooltip = "button action with a drag trigger: grab the world"}, true);
    ui::prop_separator("Speeds");
    (void)ui::prop_float("Pan speed", in.pan_speed, {.speed = 0.01F, .min = 0.0F, .max = 100.0F, .format = "%.2f"},
                         {.tooltip = "Fraction of the distance per second"});
    (void)ui::prop_float("Orbit speed", in.orbit_speed_deg, {.speed = 1.0F, .min = 0.0F, .max = 3600.0F, .format = "%.0f°/s"});
    (void)ui::prop_float("Orbit per unit", in.orbit_delta_deg, {.speed = 0.01F, .min = -100.0F, .max = 100.0F, .format = "%.2f°"});
    (void)ui::prop_float("Zoom step", in.zoom_step, {.speed = 0.005F, .min = 1.001F, .max = 4.0F, .format = "%.3f"},
                         {.tooltip = "Distance factor per wheel step"});
    (void)ui::prop_bool("Zoom to cursor", in.zoom_to_cursor);
}

} // namespace engine::editor
