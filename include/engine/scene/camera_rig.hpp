#pragma once

// Camera rig (G15.1): a generic orbit / strategy camera — the town-builder,
// RTS and orbit-viewer camera — as a component that drives its entity's
// Transform (the Camera component stays the lens). Target-space design:
// authored targets (focus, yaw, pitch, distance) are chased by exact
// critically damped springs (engine/core/spring.hpp), distance on a log scale
// so zoom feels uniform at every height.
//
// Behaviour (docs/camera-rig.md has the full description):
//   - pitch eases with zoom between pitch_near_deg and pitch_far_deg on log
//     distance (three-quarter close up, near top-down far away) when
//     pitch_follows_zoom is set — the common builder framing (Nesky, "50
//     Game Camera Mistakes", GDC 2014; Haigh-Hutchinson, "Real-Time
//     Cameras", 2009);
//   - free yaw, or yaw in steps of yaw_step_deg;
//   - pan in the yaw frame at a speed proportional to distance (keys, stick,
//     screen edges), and "grab the world" drag pan: the ground point under
//     the pointer at drag start stays under the pointer, 1:1, with release
//     inertia;
//   - zoom toward the cursor: the ground point under the cursor stays fixed;
//   - terrain awareness: the focus height follows the ground (five samples,
//     smoothed), and the camera keeps ground_clearance above the ground under
//     it by looking down more, never by moving the focus;
//   - soft bounds on the focus (rubber band while dragging, a spring back
//     after);
//   - framing a box with frame_box() (camera_framing.hpp);
//   - with a Camera on the entity: rays use its effective field of view
//     (R20 focal length), and `focus_lens` keeps its lens focused at the
//     rig's distance, so depth of field (tilt-shift) frames the focus.
//
// Conventions: yaw as camera_framing.hpp (forward = (cos p cos y, sin p,
// cos p sin y)), degrees, pitch negative looking down. The rig writes the
// entity's local transform, so the entity should be a root.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

namespace engine::input {
class Actions;
}

namespace engine::scene {

struct RigBounds {
    enum class Shape : std::uint8_t { none, rect, circle };
    Shape     shape = Shape::none;
    glm::vec2 min{-100.0F}; // rect, world XZ
    glm::vec2 max{100.0F};
    glm::vec2 center{0.0F}; // circle, world XZ
    float     radius = 100.0F;
    float     margin = 10.0F; // how far a drag may pull the focus out (rubber band)
};

// Damped current values (system-managed; `valid` false until first update).
struct RigState {
    bool      valid = false;
    glm::vec3 focus{0.0F};
    glm::vec3 focus_vel{0.0F};
    float     yaw_deg = 0.0F, yaw_vel = 0.0F;
    float     pitch_deg = 0.0F, pitch_vel = 0.0F;
    float     log_distance = 0.0F, log_distance_vel = 0.0F;
    float     ground_y = 0.0F, ground_vel = 0.0F;
    bool      has_ground = false;
    bool      snap_ground = true; // the next ground sample is taken as is (no spring)
    // Grab-the-world drag: the grabbed ground point, and the release velocity.
    bool      dragging = false;
    glm::vec3 drag_anchor{0.0F};
    glm::vec2 drag_vel{0.0F}; // world XZ, m/s
    float     yaw_accum_deg = 0.0F; // stepped yaw: rotation input not yet stepped
};

struct CameraRig {
    // Targets (authored; input and remote calls move them).
    glm::vec3 focus{0.0F};     // the ground point the camera orbits
    float     yaw_deg = 45.0F;
    float     pitch_deg = -60.0F; // used when pitch_follows_zoom is off
    float     distance = 40.0F;   // metres from the focus
    // Limits and feel.
    float min_distance = 6.0F;
    float max_distance = 250.0F;
    float pitch_near_deg = -40.0F; // at min_distance ...
    float pitch_far_deg = -65.0F;  // ... and at max_distance
    bool  pitch_follows_zoom = true;
    float min_pitch_deg = -89.0F;  // limits of a free pitch (and of clearance)
    float max_pitch_deg = -5.0F;
    float yaw_step_deg = 0.0F;     // 0: free rotation; else a divisor of 360 (45, 90) to snap
    float focus_halflife_s = 0.12F;
    float yaw_halflife_s = 0.10F;
    float zoom_halflife_s = 0.15F;
    float pitch_halflife_s = 0.15F;
    float ground_clearance = 2.0F;        // camera stays this far above the ground
    float focus_ground_halflife_s = 0.25F; // smoothing of the focus height
    bool  follow_ground = true;            // focus height follows the ground
    float drag_inertia_halflife_s = 0.15F; // release velocity decay (0 = none)
    bool  focus_lens = true; // keep the Camera lens (R20 depth of field) focused on the focus
    RigBounds bounds;
    RigState  state;
};

// Optional: G5 actions that drive the rig, by name (empty = unused). Rates
// are per second (keys, sticks, edges); deltas are per frame (mouse).
struct CameraRigInput {
    std::string pan;          // axis2d rate: x right, y forward
    std::string orbit;        // axis1d rate: positive turns right
    std::string orbit_delta;  // axis1d delta: degrees per unit (mouse:delta_x with a button)
    std::string pitch_delta;  // axis1d delta (free pitch only)
    std::string zoom;         // axis1d delta: wheel steps, positive zooms in
    std::string drag_pan;     // button with a drag trigger: grab the world
    float pan_speed = 1.0F;          // fraction of the distance per second
    float orbit_speed_deg = 90.0F;   // degrees per second at full rate
    float orbit_delta_deg = 0.25F;   // degrees per unit of orbit_delta
    float zoom_step = 1.15F;         // distance factor per wheel step
    bool  zoom_to_cursor = true;
};

// ---- Pure maths (tested directly) -------------------------------------------

[[nodiscard]] glm::vec3 rig_forward(float yaw_deg, float pitch_deg);
[[nodiscard]] glm::vec3 rig_eye(const glm::vec3& focus, float yaw_deg, float pitch_deg,
                                float distance);
// Pitch for a distance when pitch_follows_zoom is set (log-distance lerp).
[[nodiscard]] float rig_pitch_for_distance(const CameraRig& rig, float distance);
// The camera-to-world matrix of an eye looking along forward(yaw, pitch).
[[nodiscard]] glm::mat4 rig_world_matrix(const glm::vec3& eye, float yaw_deg, float pitch_deg);

// A view through the rig: where the camera is and how it projects.
struct RigView {
    glm::vec3 focus{0.0F};
    float     yaw_deg = 0.0F;
    float     pitch_deg = 0.0F;
    float     distance = 1.0F;
    float     fov_y = 1.0F;  // radians
    float     aspect = 1.0F; // width / height
};
// The world ray through a viewport pixel (`pointer` in pixels of a viewport
// `viewport_px` large, y down): {origin, unit direction}.
struct Ray {
    glm::vec3 origin{0.0F};
    glm::vec3 dir{0.0F, 0.0F, -1.0F};
};
[[nodiscard]] Ray rig_pointer_ray(const RigView& view, glm::vec2 pointer, glm::vec2 viewport_px);
// Where `ray` meets the plane y = `height` (in front of the origin).
[[nodiscard]] std::optional<glm::vec3> ray_plane_y(const Ray& ray, float height);

// The focus that puts world point `ground` under `pointer` for `view`'s
// orientation and distance, moving the focus in XZ only. Exact for any
// pitch: translating the whole camera translates the ray's hit on a
// horizontal plane by the same amount.
[[nodiscard]] glm::vec3 rig_focus_keeping_point(const RigView& view, glm::vec2 pointer,
                                                glm::vec2 viewport_px, const glm::vec3& ground);

// Zoom toward the cursor: the new target {focus, distance, pitch} after
// `steps` wheel steps (positive zooms in) that keeps `ground` (the point the
// cursor shows now) under the cursor once the rig settles.
struct ZoomResult {
    glm::vec3 focus{0.0F};
    float     distance = 1.0F;
    float     pitch_deg = 0.0F;
};
[[nodiscard]] ZoomResult rig_zoom_to_cursor(const CameraRig& rig, const RigView& target_view,
                                            float steps, float zoom_step, glm::vec2 pointer,
                                            glm::vec2 viewport_px, const glm::vec3& ground);

// Bounds: the focus clamped into the bounds (hard), or pulled toward them
// with a rubber band that never lets it go further than `margin` (soft).
[[nodiscard]] glm::vec3 rig_clamp_hard(const RigBounds& bounds, const glm::vec3& focus);
[[nodiscard]] glm::vec3 rig_clamp_soft(const RigBounds& bounds, const glm::vec3& focus);

// Height under (x, z), or nullopt where there is no ground.
using RigGroundFn = std::function<std::optional<float>(float x, float z)>;
// The most-downward-looking pitch needed so the eye (at `distance` from
// `focus` on heading `yaw_deg`) stays `clearance` above the ground; `pitch`
// itself when it is already clear. Never above `pitch`, never below
// `min_pitch_deg`.
[[nodiscard]] float rig_clear_pitch(const RigGroundFn& ground, const glm::vec3& focus,
                                    float yaw_deg, float pitch_deg, float distance,
                                    float clearance, float min_pitch_deg);

// Targets that frame the box [lo, hi] at the rig's current yaw (and pitch,
// following the distance when pitch_follows_zoom): the focus becomes the
// box centre, the distance fits it (clamped to the rig's limits).
void camera_rig_frame(CameraRig& rig, const glm::vec3& lo, const glm::vec3& hi, float fov_y,
                      float aspect, float margin = 0.1F);

// Snaps the damped state to the targets (remote `immediate`, scene load).
void camera_rig_snap(CameraRig& rig);
// The camera-to-world matrix for the rig's current (damped) state.
[[nodiscard]] glm::mat4 camera_rig_matrix(const CameraRig& rig);

// ---- System -------------------------------------------------------------------

struct RigEnvironment {
    RigGroundFn ground; // optional: terrain / ground height
    // Optional: physics ray cast for the pointer's ground point (drag pan,
    // zoom to cursor); falls back to the plane y = focus.y.
    std::function<std::optional<glm::vec3>(const glm::vec3& origin, const glm::vec3& dir,
                                           float max_distance)>
        raycast;
    const input::Actions* actions = nullptr; // for CameraRigInput
    glm::vec2 pointer{0.0F};                 // viewport pixels
    glm::vec2 viewport_px{1.0F};
};

// Runs after input, before the transform pass: applies CameraRigInput,
// steps every rig's springs by `dt` (real time: the camera moves while the
// simulation is paused) and writes the entity's Transform.local.
void camera_rig_system(entt::registry& registry, float dt, const RigEnvironment& env);

} // namespace engine::scene
