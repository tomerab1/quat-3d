// Camera rig maths (G15.1) — see include/engine/scene/camera_rig.hpp and
// docs/camera-rig.md.

#include "engine/scene/camera_rig.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/scene/camera_framing.hpp"

namespace engine::scene {

namespace {

constexpr float k_min_log_span = 1.0e-4F;

[[nodiscard]] glm::vec3 rig_right(float yaw_deg) {
    const float y = glm::radians(yaw_deg);
    return {-std::sin(y), 0.0F, std::cos(y)};
}

// Pulls `excess` metres past a bound back to at most `margin` metres, with
// slope 1 at the bound (a rubber band: the further out, the stiffer).
[[nodiscard]] float rubber_band(float excess, float margin) {
    if (excess <= 0.0F) return excess;
    if (!(margin > 0.0F)) return 0.0F;
    return margin * (1.0F - std::exp(-excess / margin));
}

} // namespace

glm::vec3 rig_forward(float yaw_deg, float pitch_deg) {
    return framing_forward(glm::radians(yaw_deg), glm::radians(pitch_deg));
}

glm::vec3 rig_eye(const glm::vec3& focus, float yaw_deg, float pitch_deg, float distance) {
    return focus - rig_forward(yaw_deg, pitch_deg) * distance;
}

float rig_pitch_for_distance(const CameraRig& rig, float distance) {
    if (!rig.pitch_follows_zoom) return std::clamp(rig.pitch_deg, rig.min_pitch_deg, rig.max_pitch_deg);
    const float lo = std::log(std::max(rig.min_distance, 1.0e-3F));
    const float hi = std::log(std::max(rig.max_distance, rig.min_distance * 1.0001F));
    const float span = std::max(hi - lo, k_min_log_span);
    const float t = std::clamp((std::log(std::max(distance, 1.0e-3F)) - lo) / span, 0.0F, 1.0F);
    return rig.pitch_near_deg + (rig.pitch_far_deg - rig.pitch_near_deg) * t;
}

glm::mat4 rig_world_matrix(const glm::vec3& eye, float yaw_deg, float pitch_deg) {
    // Built from the heading, not lookAt: well defined even looking straight
    // down, where forward is parallel to world up.
    const glm::vec3 f = rig_forward(yaw_deg, pitch_deg);
    const glm::vec3 r = rig_right(yaw_deg);
    const glm::vec3 u = glm::cross(r, f);
    glm::mat4 m(1.0F);
    m[0] = glm::vec4(r, 0.0F);
    m[1] = glm::vec4(u, 0.0F);
    m[2] = glm::vec4(-f, 0.0F); // cameras look down -Z
    m[3] = glm::vec4(eye, 1.0F);
    return m;
}

Ray rig_pointer_ray(const RigView& view, glm::vec2 pointer, glm::vec2 viewport_px) {
    const glm::vec2 size = glm::max(viewport_px, glm::vec2(1.0F));
    const glm::vec2 ndc(2.0F * pointer.x / size.x - 1.0F, 1.0F - 2.0F * pointer.y / size.y);
    const float t = std::tan(view.fov_y * 0.5F);
    const glm::vec3 f = rig_forward(view.yaw_deg, view.pitch_deg);
    const glm::vec3 r = rig_right(view.yaw_deg);
    const glm::vec3 u = glm::cross(r, f);
    Ray ray;
    ray.origin = rig_eye(view.focus, view.yaw_deg, view.pitch_deg, view.distance);
    ray.dir = glm::normalize(f + r * (ndc.x * t * view.aspect) + u * (ndc.y * t));
    return ray;
}

std::optional<glm::vec3> ray_plane_y(const Ray& ray, float height) {
    if (std::abs(ray.dir.y) < 1.0e-6F) return std::nullopt;
    const float t = (height - ray.origin.y) / ray.dir.y;
    if (!(t > 0.0F) || !std::isfinite(t)) return std::nullopt;
    return ray.origin + ray.dir * t;
}

glm::vec3 rig_focus_keeping_point(const RigView& view, glm::vec2 pointer, glm::vec2 viewport_px,
                                  const glm::vec3& ground) {
    const auto hit = ray_plane_y(rig_pointer_ray(view, pointer, viewport_px), ground.y);
    if (!hit) return view.focus; // the pointer is above the horizon
    return {view.focus.x + ground.x - hit->x, view.focus.y, view.focus.z + ground.z - hit->z};
}

ZoomResult rig_zoom_to_cursor(const CameraRig& rig, const RigView& target_view, float steps,
                              float zoom_step, glm::vec2 pointer, glm::vec2 viewport_px,
                              const glm::vec3& ground) {
    ZoomResult out;
    out.distance = std::clamp(target_view.distance * std::pow(std::max(zoom_step, 1.0001F), -steps),
                              rig.min_distance, rig.max_distance);
    out.pitch_deg = rig.pitch_follows_zoom ? rig_pitch_for_distance(rig, out.distance) : target_view.pitch_deg;
    RigView after = target_view;
    after.distance = out.distance;
    after.pitch_deg = out.pitch_deg;
    out.focus = rig_focus_keeping_point(after, pointer, viewport_px, ground);
    return out;
}

glm::vec3 rig_clamp_hard(const RigBounds& b, const glm::vec3& focus) {
    glm::vec3 out = focus;
    switch (b.shape) {
    case RigBounds::Shape::none: break;
    case RigBounds::Shape::rect:
        out.x = std::clamp(focus.x, std::min(b.min.x, b.max.x), std::max(b.min.x, b.max.x));
        out.z = std::clamp(focus.z, std::min(b.min.y, b.max.y), std::max(b.min.y, b.max.y));
        break;
    case RigBounds::Shape::circle: {
        const glm::vec2 d(focus.x - b.center.x, focus.z - b.center.y);
        const float len = glm::length(d);
        if (len > b.radius && len > 0.0F) {
            const glm::vec2 p = b.center + d * (std::max(b.radius, 0.0F) / len);
            out.x = p.x;
            out.z = p.y;
        }
        break;
    }
    }
    return out;
}

glm::vec3 rig_clamp_soft(const RigBounds& b, const glm::vec3& focus) {
    glm::vec3 out = focus;
    switch (b.shape) {
    case RigBounds::Shape::none: break;
    case RigBounds::Shape::rect: {
        const auto axis = [&](float v, float lo, float hi) {
            if (v > hi) return hi + rubber_band(v - hi, b.margin);
            if (v < lo) return lo - rubber_band(lo - v, b.margin);
            return v;
        };
        out.x = axis(focus.x, std::min(b.min.x, b.max.x), std::max(b.min.x, b.max.x));
        out.z = axis(focus.z, std::min(b.min.y, b.max.y), std::max(b.min.y, b.max.y));
        break;
    }
    case RigBounds::Shape::circle: {
        const glm::vec2 d(focus.x - b.center.x, focus.z - b.center.y);
        const float len = glm::length(d);
        if (len > b.radius && len > 0.0F) {
            const float r = b.radius + rubber_band(len - b.radius, b.margin);
            const glm::vec2 p = b.center + d * (r / len);
            out.x = p.x;
            out.z = p.y;
        }
        break;
    }
    }
    return out;
}

float rig_clear_pitch(const RigGroundFn& ground, const glm::vec3& focus, float yaw_deg,
                      float pitch_deg, float distance, float clearance, float min_pitch_deg) {
    if (!ground || !(distance > 0.0F)) return pitch_deg;
    float pitch = pitch_deg;
    // Looking further down moves the eye toward the focus, onto other
    // ground: a few fixed-point rounds settle it.
    for (int round = 0; round < 6; ++round) {
        const glm::vec3 eye = rig_eye(focus, yaw_deg, pitch, distance);
        const auto g = ground(eye.x, eye.z);
        if (!g) break;
        const float need = *g + clearance;
        if (eye.y >= need - 1.0e-4F) break;
        // eye.y = focus.y - sin(pitch) * distance >= need.
        const float s = (need - focus.y) / distance;
        const float required = s >= 1.0F ? -90.0F : glm::degrees(-std::asin(std::max(s, -1.0F)));
        pitch = std::max(std::min(pitch, required), min_pitch_deg);
        if (pitch <= min_pitch_deg) break;
    }
    return pitch;
}

void camera_rig_frame(CameraRig& rig, const glm::vec3& lo, const glm::vec3& hi, float fov_y,
                      float aspect, float margin) {
    const glm::vec3 centre = (lo + hi) * 0.5F;
    float distance = std::clamp(rig.distance, rig.min_distance, rig.max_distance);
    // With pitch following zoom, pitch and distance depend on each other: a
    // few rounds of "pitch for this distance, distance for this pitch".
    const int rounds = rig.pitch_follows_zoom ? 6 : 1;
    for (int i = 0; i < rounds; ++i) {
        const float pitch = rig_pitch_for_distance(rig, distance);
        FramingParams p;
        p.yaw = glm::radians(rig.yaw_deg);
        p.pitch = glm::radians(pitch);
        p.fov_y = fov_y;
        p.aspect = aspect;
        p.margin = margin;
        const glm::vec3 eye = frame_box(lo, hi, p);
        distance = std::clamp(glm::length(eye - centre), rig.min_distance, rig.max_distance);
    }
    rig.focus = centre;
    rig.distance = distance;
}

void camera_rig_snap(CameraRig& rig) {
    RigState& s = rig.state;
    const bool had_ground = s.valid && s.has_ground;
    const float ground_y = s.ground_y;
    s = RigState{};
    s.valid = true;
    s.focus = rig.focus;
    s.yaw_deg = rig.yaw_deg;
    s.log_distance = std::log(std::clamp(rig.distance, std::max(rig.min_distance, 1.0e-3F), rig.max_distance));
    s.pitch_deg = rig_pitch_for_distance(rig, std::exp(s.log_distance));
    // The system re-samples the ground under the new focus and takes it
    // without smoothing (snap_ground), so a snapped pose is exact.
    s.has_ground = had_ground;
    s.ground_y = had_ground ? ground_y : rig.focus.y;
    s.snap_ground = true;
}

glm::mat4 camera_rig_matrix(const CameraRig& rig) {
    const RigState& s = rig.state;
    const glm::vec3 eye = rig_eye(s.focus, s.yaw_deg, s.pitch_deg, std::exp(s.log_distance));
    return rig_world_matrix(eye, s.yaw_deg, s.pitch_deg);
}

} // namespace engine::scene
