#include "app/fly_camera.hpp"

#include <cmath>
#include <limits>

#include <SDL3/SDL.h>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/asset/mesh_asset.hpp"
#include "engine/scene/components.hpp"

namespace quat::app {
namespace {

const glm::vec3 k_world_up(0.0F, 1.0F, 0.0F);

// World-space AABB over every loaded renderable (mesh local bounds transformed by
// its world matrix). Returns false if the scene has no loaded geometry. Tick the
// scene first so world transforms are current.
bool scene_world_bounds(engine::scene::Scene& scene, glm::vec3& out_min, glm::vec3& out_max) {
    out_min = glm::vec3(std::numeric_limits<float>::max());
    out_max = glm::vec3(std::numeric_limits<float>::lowest());
    bool any = false;
    for (auto [e, t, mr] :
         scene.registry().view<engine::scene::Transform, engine::scene::MeshRenderer>().each()) {
        if (!mr.mesh.valid() || !mr.mesh.is_loaded()) continue;
        // Skinned meshes render with an identity model (their skinning matrices
        // already place the vertices), so frame their bind bounds through the
        // same armature world the animation system uses: entity world times the
        // entity-relative armature offset.
        const auto* sm = scene.registry().try_get<engine::scene::SkinnedMesh>(e);
        const glm::mat4 model = sm != nullptr ? t.world * sm->root_transform : t.world;
        const engine::asset::Aabb& ab = mr.mesh->bounds;
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 corner((i & 1) ? ab.max.x : ab.min.x, (i & 2) ? ab.max.y : ab.min.y,
                                   (i & 4) ? ab.max.z : ab.min.z);
            const glm::vec3 w = glm::vec3(model * glm::vec4(corner, 1.0F));
            out_min = glm::min(out_min, w);
            out_max = glm::max(out_max, w);
            any = true;
        }
    }
    return any;
}

// Camera world transform that frames the given AABB: looks at its centre from a
// distance sized so the bounding sphere fits the vertical field of view.
glm::mat4 frame_camera_world(const glm::vec3& bmin, const glm::vec3& bmax, float fov_y) {
    const glm::vec3 center = 0.5F * (bmin + bmax);
    const float radius = glm::max(0.001F, 0.5F * glm::length(bmax - bmin));
    const float dist = radius / std::tan(fov_y * 0.5F) * 1.6F;
    const glm::vec3 eye = center + glm::normalize(glm::vec3(0.6F, 0.45F, 1.0F)) * dist;
    return glm::inverse(glm::lookAt(eye, center, glm::vec3(0.0F, 1.0F, 0.0F)));
}

// One movement axis from a key pair: +1, -1 or 0 (both or neither held).
float axis(const bool* keys, bool ok, SDL_Scancode pos, SDL_Scancode neg) {
    if (!ok) return 0.0F;
    return (keys[pos] ? 1.0F : 0.0F) - (keys[neg] ? 1.0F : 0.0F);
}

} // namespace

FlyCamera FlyCamera::create(engine::scene::Scene& scene, bool physics_mode) {
    FlyCamera cam;
    // Non-editor builds keep a scene camera entity driven by the fly input
    // (camera_system is their only view path). Editor builds own the fly
    // camera as editor state instead — scene Camera entities are the user's.
#ifndef ENGINE_EDITOR
    cam.camera_entity_ = scene.create_entity("camera");
    scene.registry().emplace<engine::scene::Camera>(cam.camera_entity_);
#endif
    const engine::scene::Camera default_camera{};
    scene.tick(); // resolve world transforms before measuring the scene bounds
    glm::vec3 bmin{};
    glm::vec3 bmax{};
    const bool has_geo = scene_world_bounds(scene, bmin, bmax);
    const glm::mat4 framed =
        has_geo ? frame_camera_world(bmin, bmax, default_camera.fov_y)
                : glm::inverse(glm::lookAt(glm::vec3(3.0F, 2.5F, 4.0F), glm::vec3(0.0F),
                                           glm::vec3(0.0F, 1.0F, 0.0F)));

    // Free-fly camera state, seeded from the framed transform so the opening view
    // matches the auto-frame. Updated each frame from input (WASD + right-drag).
    // The physics demo's huge floor would push the auto-frame far away; use a
    // fixed view of the drop zone instead.
    const glm::mat4 framed_view =
        physics_mode ? glm::inverse(glm::lookAt(glm::vec3(8.0F, 5.0F, 10.0F),
                                                glm::vec3(0.0F, 1.5F, 0.0F),
                                                glm::vec3(0.0F, 1.0F, 0.0F)))
                     : framed;
    cam.pos = glm::vec3(framed_view[3]);
    const glm::vec3 cam_fwd0 = -glm::normalize(glm::vec3(framed_view[2])); // camera looks down -Z
    cam.yaw = std::atan2(cam_fwd0.z, cam_fwd0.x);
    cam.pitch = std::asin(glm::clamp(cam_fwd0.y, -0.999F, 0.999F));
    cam.fov_y = default_camera.fov_y;
    cam.move_speed_ = physics_mode ? 6.0F
                      : has_geo    ? glm::max(1.0F, 0.5F * glm::length(bmax - bmin))
                                   : 3.0F;
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
    const float speed = move_speed_ * (keys[SDL_SCANCODE_LSHIFT] ? 4.0F : 1.0F);
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

glm::vec3 FlyCamera::forward() const {
    return glm::vec3(std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                     std::cos(pitch) * std::sin(yaw));
}

// View/projection for the free-fly camera. In editor builds the fly camera is
// editor state (per CLAUDE.md it never lives in the ECS): scene Camera entities
// belong to the user's scene, keep their authored transforms, and take over the
// view only in play mode. Conventions mirror camera_system (Vulkan Y flip).
engine::scene::CameraMatrices FlyCamera::matrices(float aspect) const {
    const glm::vec3 fwd = forward();
    const engine::scene::Camera defaults{};
    engine::scene::CameraMatrices out;
    out.position = pos;
    out.view = glm::lookAt(pos, pos + fwd, glm::vec3(0.0F, 1.0F, 0.0F));
    out.projection = glm::perspective(fov_y, aspect, defaults.near_z, defaults.far_z);
    out.projection[1][1] *= -1.0F; // Vulkan clip space: Y points down
    out.view_proj = out.projection * out.view;
    return out;
}

} // namespace quat::app
