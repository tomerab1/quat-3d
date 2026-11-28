#include "app/fly_camera.hpp"

#include <cmath>
#include <optional>

#include <SDL3/SDL.h>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/asset/mesh_asset.hpp"
#include "engine/scene/bounds.hpp"
#include "engine/scene/camera_framing.hpp"
#include "engine/scene/components.hpp"

namespace quat::app {
namespace {

const glm::vec3 k_world_up(0.0F, 1.0F, 0.0F);

// World-space bounds of everything that draws (engine/scene/bounds.hpp:
// meshes, skinned meshes through their armature, unresolved primitives,
// instanced placements), minus `exclude`. nullopt when nothing draws. Tick the
// scene first so world transforms are current.
std::optional<engine::asset::Aabb> framing_bounds(const engine::scene::Scene& scene,
                                                  entt::entity exclude) {
    const entt::registry& r = scene.registry();
    engine::asset::Aabb box = engine::scene::empty_bounds();
    for (const auto [e, t] : r.view<const engine::scene::Transform>().each()) {
        if (e == exclude) continue;
        if (const auto local = engine::scene::local_draw_bounds(r, e)) {
            engine::scene::merge_bounds(box, engine::scene::transform_bounds(*local, t.world));
        }
    }
    if (engine::scene::bounds_empty(box)) return std::nullopt;
    return box;
}

// The opening view's heading and pitch: from the front-right (+Z, +X) of the
// content, looking down 25 degrees.
const float k_frame_yaw = std::atan2(-1.0F, -0.5F);
const float k_frame_pitch = glm::radians(-25.0F);
constexpr float k_frame_margin = 0.1F;

// One movement axis from a key pair: +1, -1 or 0 (both or neither held).
float axis(const bool* keys, bool ok, SDL_Scancode pos, SDL_Scancode neg) {
    if (!ok) return 0.0F;
    return (keys[pos] ? 1.0F : 0.0F) - (keys[neg] ? 1.0F : 0.0F);
}

} // namespace

FlyCamera FlyCamera::create(engine::scene::Scene& scene, const Start& start) {
    FlyCamera cam;
    // Non-editor builds keep a scene camera entity driven by the fly input
    // (camera_system is their only view path). Editor builds own the fly
    // camera as editor state instead — scene Camera entities are the user's.
#ifndef ENGINE_EDITOR
    cam.camera_entity_ = scene.create_entity("camera");
    scene.registry().emplace<engine::scene::Camera>(cam.camera_entity_);
#endif
    const engine::scene::Camera default_camera{};
    cam.fov_y = default_camera.fov_y;
    if (start.physics_mode) {
        // The physics demo's huge floor would push the framing far away; use
        // a fixed view of the drop zone instead.
        const glm::vec3 eye(8.0F, 5.0F, 10.0F);
        const glm::vec3 fwd = glm::normalize(glm::vec3(0.0F, 1.5F, 0.0F) - eye);
        cam.pos = eye;
        cam.yaw = std::atan2(fwd.z, fwd.x);
        cam.pitch = std::asin(glm::clamp(fwd.y, -0.999F, 0.999F));
        cam.move_speed_ = 6.0F;
        return cam;
    }

    scene.tick(); // resolve world transforms before measuring the scene bounds
    const std::optional<engine::asset::Aabb> bounds = framing_bounds(scene, start.exclude);
    const bool has_geo = bounds.has_value();
    // Nothing to frame: look at the origin.
    const glm::vec3 bmin = has_geo ? bounds->min : glm::vec3(-1.0F);
    const glm::vec3 bmax = has_geo ? bounds->max : glm::vec3(1.0F);
    engine::scene::FramingParams framing;
    framing.yaw = k_frame_yaw;
    framing.pitch = k_frame_pitch;
    framing.fov_y = default_camera.fov_y;
    framing.aspect = start.aspect;
    framing.margin = k_frame_margin;
    framing.near_z = default_camera.near_z;
    cam.pos = engine::scene::frame_box(bmin, bmax, framing);
    cam.yaw = k_frame_yaw;
    cam.pitch = k_frame_pitch;
    cam.move_speed_ = has_geo ? glm::max(1.0F, 0.5F * glm::length(bmax - bmin)) : 3.0F;
    return cam;
}

void FlyCamera::begin_input() {
    look_dx_ = 0.0F;
    look_dy_ = 0.0F;
}

void FlyCamera::handle_event(const SDL_Event& event, SDL_Window* window, bool accept_clicks) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        // Camera input belongs to the Viewport panel; clicks anywhere else
        // are the UI's.
        if (!accept_clicks) break;
        if (event.button.button == SDL_BUTTON_RIGHT) {
            looking_ = true;
            SDL_SetWindowRelativeMouseMode(window, true);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_RIGHT) {
            looking_ = false;
            SDL_SetWindowRelativeMouseMode(window, false);
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (looking_) {
            look_dx_ += event.motion.xrel;
            look_dy_ += event.motion.yrel;
        }
        break;
    default:
        break;
    }
}

void FlyCamera::apply_look() {
    constexpr float look_sensitivity = 0.0025F;
    yaw += look_dx_ * look_sensitivity;
    pitch = glm::clamp(pitch - look_dy_ * look_sensitivity, glm::radians(-89.0F),
                       glm::radians(89.0F));
}

glm::vec3 FlyCamera::character_wish(FlyInputGate gate) const {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    // WASD drives the character along the camera's heading (third-person).
    const glm::vec3 fwd_h = glm::normalize(glm::vec3(std::cos(yaw), 0.0F, std::sin(yaw)));
    const glm::vec3 right_h = glm::normalize(glm::cross(fwd_h, k_world_up));
    const glm::vec3 wish =
        fwd_h * (axis(keys, gate.wasd, SDL_SCANCODE_W, SDL_SCANCODE_S) +
                 axis(keys, gate.arrows, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN)) +
        right_h * (axis(keys, gate.wasd, SDL_SCANCODE_D, SDL_SCANCODE_A) +
                   axis(keys, gate.arrows, SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT));
    return wish != glm::vec3(0.0F) ? glm::normalize(wish) : glm::vec3(0.0F);
}

void FlyCamera::fly([[maybe_unused]] engine::scene::Scene& scene, FlyInputGate gate, float dt) {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    // Free-fly camera: right-drag looks, WASD moves, Q/E down/up, Shift fast.
    const glm::vec3 cam_fwd = forward();
    const glm::vec3 cam_right = glm::normalize(glm::cross(cam_fwd, k_world_up));
    const glm::vec3 move =
        cam_fwd * (axis(keys, gate.wasd, SDL_SCANCODE_W, SDL_SCANCODE_S) +
                   axis(keys, gate.arrows, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN)) +
        cam_right * (axis(keys, gate.wasd, SDL_SCANCODE_D, SDL_SCANCODE_A) +
                     axis(keys, gate.arrows, SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT)) +
        k_world_up * (axis(keys, gate.wasd, SDL_SCANCODE_E, SDL_SCANCODE_Q) +
                      axis(keys, gate.arrows, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEDOWN));
    const float speed = move_speed_ * speed_scale * (keys[SDL_SCANCODE_LSHIFT] ? 4.0F : 1.0F);
    if (move != glm::vec3(0.0F)) {
        pos += glm::normalize(move) * speed * dt;
    }
#ifndef ENGINE_EDITOR
    // try_get: the editor can delete any entity, including the camera.
    if (auto* cam_tf = scene.registry().valid(camera_entity_)
                           ? scene.registry().try_get<engine::scene::Transform>(camera_entity_)
                           : nullptr) {
        cam_tf->local = glm::inverse(glm::lookAt(pos, pos + cam_fwd, k_world_up));
    }
#endif
}

void FlyCamera::follow(engine::scene::Scene& scene, entt::entity target_entity) {
    // Third-person follow camera, placed after the character has moved.
    // try_get: the editor can delete the character entity.
    if (!scene.registry().valid(target_entity)) return;
    const auto* char_tf = scene.registry().try_get<engine::scene::Transform>(target_entity);
    if (char_tf == nullptr) return;
    const glm::vec3 target = glm::vec3(char_tf->world[3]) + glm::vec3(0.0F, 0.8F, 0.0F);
    pos = target - forward() * 5.0F;
#ifndef ENGINE_EDITOR
    if (auto* camera_tf = scene.registry().valid(camera_entity_)
                              ? scene.registry().try_get<engine::scene::Transform>(camera_entity_)
                              : nullptr) {
        const glm::mat4 view_inv = glm::inverse(glm::lookAt(pos, target, k_world_up));
        camera_tf->local = view_inv;
        camera_tf->world = view_inv;
    }
#endif
}

engine::scene::ClipRange FlyCamera::clip() const {
    return engine::scene::apply_clip_overrides(auto_clip_, near_override, far_override);
}

void FlyCamera::set_derived_clip([[maybe_unused]] engine::scene::Scene& scene,
                                 const engine::scene::ClipRange& derived) {
    auto_clip_ = derived;
#ifndef ENGINE_EDITOR
    // Without the editor the view renders through this camera's entity.
    entt::registry& r = scene.registry();
    if (auto* c = r.valid(camera_entity_) ? r.try_get<engine::scene::Camera>(camera_entity_)
                                          : nullptr) {
        const engine::scene::ClipRange planes = clip();
        c->near_z = planes.near_z;
        c->far_z = planes.far_z;
    }
#endif
}

glm::vec3 FlyCamera::forward() const {
    return glm::vec3(std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                     std::cos(pitch) * std::sin(yaw));
}

// View/projection for the free-fly camera. In editor builds the fly camera is
// editor state (per CLAUDE.md it never lives in the ECS): scene Camera entities
// belong to the user's scene, keep their authored transforms, and take over the
// view only in play mode. Conventions mirror camera_system (Vulkan Y flip).
engine::scene::CameraMatrices FlyCamera::matrices(float aspect) const {
    return view_matrices(pos, yaw, pitch, fov_y, clip(), aspect);
}

engine::scene::CameraMatrices view_matrices(const glm::vec3& pos, float yaw, float pitch,
                                            float fov_y, const engine::scene::ClipRange& clip,
                                            float aspect) {
    const glm::vec3 fwd(std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                        std::cos(pitch) * std::sin(yaw));
    engine::scene::CameraMatrices out;
    out.position = pos;
    out.view = glm::lookAt(pos, pos + fwd, k_world_up);
    out.projection = glm::perspective(fov_y, aspect, clip.near_z, clip.far_z);
    out.projection[1][1] *= -1.0F; // Vulkan clip space: Y points down
    out.view_proj = out.projection * out.view;
    return out;
}

} // namespace quat::app
