// camera_rig_system (G15.1): input, springs, ground and bounds per rig — see
// include/engine/scene/camera_rig.hpp and docs/camera-rig.md.

#include <algorithm>
#include <array>
#include <cmath>

#include <entt/entity/registry.hpp>

#include "engine/core/spring.hpp"
#include "engine/input/actions.hpp"
#include "engine/scene/camera_rig.hpp"
#include "engine/scene/components.hpp"

namespace engine::scene {

namespace {

constexpr float k_default_fov = 1.0471976F; // 60 degrees
constexpr float k_inertia_stop = 1.0e-3F;   // m/s

[[nodiscard]] const input::ActionState* action(const RigEnvironment& env, const std::string& name) {
    if (env.actions == nullptr || name.empty()) return nullptr;
    return env.actions->state(name);
}

[[nodiscard]] float effective_pitch(const CameraRig& rig, float distance) {
    return rig_pitch_for_distance(rig, distance);
}

[[nodiscard]] RigView current_view(const CameraRig& rig, float fov_y, float aspect) {
    return RigView{rig.state.focus, rig.state.yaw_deg, rig.state.pitch_deg,
                   std::exp(rig.state.log_distance), fov_y, aspect};
}

// The ground point the pointer shows in `view`: physics first, else the
// plane through the focus.
[[nodiscard]] std::optional<glm::vec3> pointer_ground(const RigEnvironment& env, const RigView& view,
                                                      glm::vec2 pointer, float max_distance) {
    const Ray ray = rig_pointer_ray(view, pointer, env.viewport_px);
    if (env.raycast) {
        if (auto hit = env.raycast(ray.origin, ray.dir, max_distance)) return hit;
    }
    return ray_plane_y(ray, view.focus.y);
}

// Mean ground height at the focus and four neighbours (so single bumps do
// not jolt the view), or nullopt where there is no ground at all.
[[nodiscard]] std::optional<float> ground_under_focus(const RigGroundFn& ground, glm::vec3 focus,
                                                      float distance) {
    const float r = std::clamp(distance * 0.05F, 2.0F, 12.0F);
    const std::array<glm::vec2, 5> offsets = {glm::vec2(0.0F), glm::vec2(r, 0.0F), glm::vec2(-r, 0.0F),
                                              glm::vec2(0.0F, r), glm::vec2(0.0F, -r)};
    float sum = 0.0F;
    int n = 0;
    for (const glm::vec2& o : offsets) {
        if (const auto h = ground(focus.x + o.x, focus.z + o.y)) {
            sum += *h;
            ++n;
        }
    }
    if (n == 0) return std::nullopt;
    return sum / static_cast<float>(n);
}

void apply_rotation(CameraRig& rig, const CameraRigInput& in, const RigEnvironment& env, float dt) {
    float rate = 0.0F;
    bool  rate_started = false;
    if (const auto* s = action(env, in.orbit)) {
        rate = s->value.x;
        rate_started = s->started;
    }
    float delta = 0.0F;
    if (const auto* s = action(env, in.orbit_delta)) delta = s->value.x * in.orbit_delta_deg;
    if (rig.yaw_step_deg > 0.0F) {
        // Stepped: a press steps once; a drag steps each time it has turned
        // half a step.
        const float step = rig.yaw_step_deg;
        if (rate_started && rate != 0.0F) rig.yaw_deg += std::copysign(step, rate);
        rig.state.yaw_accum_deg += delta;
        while (std::abs(rig.state.yaw_accum_deg) >= step * 0.5F) {
            const float dir = std::copysign(1.0F, rig.state.yaw_accum_deg);
            rig.yaw_deg += dir * step;
            rig.state.yaw_accum_deg -= dir * step;
        }
        rig.yaw_deg = std::round(rig.yaw_deg / step) * step;
    } else {
        rig.yaw_deg += rate * in.orbit_speed_deg * dt + delta;
    }
    rig.yaw_deg = core::wrap_degrees(rig.yaw_deg);
    if (!rig.pitch_follows_zoom) {
        if (const auto* s = action(env, in.pitch_delta)) {
            rig.pitch_deg = std::clamp(rig.pitch_deg + s->value.x * in.orbit_delta_deg,
                                       rig.min_pitch_deg, rig.max_pitch_deg);
        }
    }
}

void apply_input(CameraRig& rig, const CameraRigInput& in, const RigEnvironment& env, float dt,
                 float fov_y, float aspect) {
    RigState& st = rig.state;
    const RigView view = current_view(rig, fov_y, aspect);
    const float reach = rig.max_distance * 20.0F;

    // Pan (rates): in the yaw frame, proportional to the distance.
    if (const auto* s = action(env, in.pan); s != nullptr && s->value != glm::vec2(0.0F)) {
        const float y = glm::radians(rig.yaw_deg);
        const glm::vec2 fwd(std::cos(y), std::sin(y));
        const glm::vec2 right(-fwd.y, fwd.x);
        const glm::vec2 d = (right * s->value.x + fwd * s->value.y) * (in.pan_speed * rig.distance * dt);
        rig.focus.x += d.x;
        rig.focus.z += d.y;
        st.drag_vel = glm::vec2(0.0F);
    }

    apply_rotation(rig, in, env, dt);

    // Zoom toward the cursor: the ground point it shows now stays under it.
    if (const auto* s = action(env, in.zoom); s != nullptr && s->value.x != 0.0F) {
        const float steps = s->value.x;
        const auto ground = in.zoom_to_cursor ? pointer_ground(env, view, env.pointer, reach) : std::nullopt;
        if (ground) {
            RigView target{glm::vec3(rig.focus.x, st.focus.y, rig.focus.z), rig.yaw_deg,
                           effective_pitch(rig, rig.distance), rig.distance, fov_y, aspect};
            const ZoomResult z = rig_zoom_to_cursor(rig, target, steps, in.zoom_step, env.pointer,
                                                    env.viewport_px, *ground);
            rig.focus.x = z.focus.x;
            rig.focus.z = z.focus.z;
            rig.distance = z.distance;
        } else {
            rig.distance = std::clamp(rig.distance * std::pow(std::max(in.zoom_step, 1.0001F), -steps),
                                      rig.min_distance, rig.max_distance);
        }
    }

    // Grab the world: the point grabbed at the press stays under the pointer.
    const auto* drag = action(env, in.drag_pan);
    if (drag != nullptr && drag->started) {
        if (const auto anchor = pointer_ground(env, view, drag->drag_origin, reach)) {
            st.dragging = true;
            st.drag_anchor = *anchor;
            st.drag_vel = glm::vec2(0.0F);
        }
    }
    if (st.dragging && (drag == nullptr || !drag->active)) st.dragging = false; // released: inertia
    if (st.dragging) {
        const glm::vec3 moved = rig_clamp_soft(
            rig.bounds, rig_focus_keeping_point(view, env.pointer, env.viewport_px, st.drag_anchor));
        if (dt > 0.0F) {
            const glm::vec2 v((moved.x - st.focus.x) / dt, (moved.z - st.focus.z) / dt);
            st.drag_vel = glm::mix(st.drag_vel, v, 0.5F); // the release velocity, lightly filtered
        }
        st.focus.x = rig.focus.x = moved.x;
        st.focus.z = rig.focus.z = moved.z;
        st.focus_vel.x = st.focus_vel.z = 0.0F;
    }
}

void step_rig(CameraRig& rig, const RigEnvironment& env, float dt) {
    RigState& st = rig.state;
    // Inertia after a drag.
    if (!st.dragging && glm::length(st.drag_vel) > k_inertia_stop) {
        rig.focus.x += st.drag_vel.x * dt;
        rig.focus.z += st.drag_vel.y * dt;
        const float h = rig.drag_inertia_halflife_s;
        st.drag_vel *= h > 0.0F ? std::exp2(-dt / h) : 0.0F;
    } else if (!st.dragging) {
        st.drag_vel = glm::vec2(0.0F);
    }
    rig.distance = std::clamp(rig.distance, rig.min_distance, rig.max_distance);
    if (!st.dragging) rig.focus = rig_clamp_hard(rig.bounds, rig.focus); // springs back after a drag

    if (!st.dragging) {
        core::spring_damper(st.focus.x, st.focus_vel.x, rig.focus.x, rig.focus_halflife_s, dt);
        core::spring_damper(st.focus.z, st.focus_vel.z, rig.focus.z, rig.focus_halflife_s, dt);
    }
    core::spring_damper_degrees(st.yaw_deg, st.yaw_vel, rig.yaw_deg, rig.yaw_halflife_s, dt);
    st.yaw_deg = core::wrap_degrees(st.yaw_deg);
    core::spring_damper(st.log_distance, st.log_distance_vel, std::log(rig.distance), rig.zoom_halflife_s, dt);
    const float distance = std::exp(st.log_distance);

    // Focus height: the ground under the focus (smoothed), else the target.
    const auto g = rig.follow_ground && env.ground ? ground_under_focus(env.ground, st.focus, distance)
                                                   : std::nullopt;
    if (g) {
        if (st.snap_ground || !st.has_ground) {
            st.ground_y = *g;
            st.ground_vel = 0.0F;
        } else {
            core::spring_damper(st.ground_y, st.ground_vel, *g, rig.focus_ground_halflife_s, dt);
        }
        st.has_ground = true;
        st.focus.y = st.ground_y;
        st.focus_vel.y = 0.0F;
    } else {
        st.has_ground = false;
        core::spring_damper(st.focus.y, st.focus_vel.y, rig.focus.y, rig.focus_halflife_s, dt);
    }
    st.snap_ground = false;

    // Pitch: eased with the (damped) distance, then kept clear of the ground
    // by looking further down — the focus never moves for it.
    core::spring_damper(st.pitch_deg, st.pitch_vel, effective_pitch(rig, distance), rig.pitch_halflife_s, dt);
    if (env.ground) {
        const float clear = rig_clear_pitch(env.ground, st.focus, st.yaw_deg, st.pitch_deg, distance,
                                            rig.ground_clearance, rig.min_pitch_deg);
        if (clear < st.pitch_deg) {
            st.pitch_deg = clear;
            st.pitch_vel = std::min(st.pitch_vel, 0.0F);
        }
    }
}

} // namespace

void camera_rig_system(entt::registry& registry, float dt, const RigEnvironment& env) {
    if (!(dt >= 0.0F) || !std::isfinite(dt)) dt = 0.0F;
    const float aspect = env.viewport_px.y > 0.0F ? env.viewport_px.x / env.viewport_px.y : 1.0F;
    auto view = registry.view<CameraRig, Transform>();
    for (const entt::entity e : view) {
        CameraRig& rig = view.get<CameraRig>(e);
        if (!rig.state.valid) camera_rig_snap(rig);
        const Camera* cam = registry.try_get<Camera>(e);
        const float fov = cam != nullptr ? cam->fov_y : k_default_fov;
        if (const auto* in = registry.try_get<CameraRigInput>(e); in != nullptr && env.actions != nullptr) {
            apply_input(rig, *in, env, dt, fov, aspect);
        }
        step_rig(rig, env, dt);
        view.get<Transform>(e).local = camera_rig_matrix(rig);
    }
}

} // namespace engine::scene
