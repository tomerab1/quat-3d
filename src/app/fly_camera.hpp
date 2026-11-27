#pragma once

// FlyCamera — the `game` executable's free-fly view camera (app layer).
//
// Right-drag looks, WASD moves, Q/E down/up, Shift = fast (the arrow keys and
// PageUp/PageDown mirror them). In editor builds the camera is editor state
// (per CLAUDE.md it never lives in the ECS): scene Camera entities belong to
// the user's scene and take over the view only in play mode. Builds without
// the editor keep a scene "camera" entity that this camera drives, since
// camera_system is their only view path.
//
// The remote bridge's camera commands write pos / yaw / pitch / fov_y
// directly; all angles are radians.

#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>

#include "engine/scene/scene.hpp" // CameraMatrices

struct SDL_Window;
union SDL_Event;

namespace quat::app {

// Which key groups may drive movement this frame (the editor freezes them
// while the UI owns the keyboard).
struct FlyInputGate {
    bool wasd = true;
    bool arrows = true;
};

class FlyCamera {
public:
    // How the opening view is chosen.
    struct Start {
        bool  physics_mode = false; // fixed view of the physics demo's drop zone
        float aspect = 16.0F / 9.0F; // of the view the scene renders into
        // Left out of the framed bounds (the showcase's pedestal).
        entt::entity exclude = entt::null;
    };
    // Frames the scene's renderables (ticks the scene first so world
    // transforms are current) from a fixed heading, looking down 25 degrees,
    // as close as fits them with a 10% margin (scene::frame_box) — the same
    // view every run. The physics demo gets a fixed view of its drop zone
    // instead. Builds without the editor also create the scene camera entity
    // here.
    [[nodiscard]] static FlyCamera create(engine::scene::Scene& scene, const Start& start);

    // Per frame, before the event pump: clears the accumulated mouse look.
    void begin_input();
    // Mouse-look events (right button + motion). `accept_clicks` is false
    // while the pointer is over editor UI rather than the viewport.
    void handle_event(const SDL_Event& event, SDL_Window* window, bool accept_clicks);
    // Applies the accumulated mouse look to yaw / pitch.
    void apply_look();

    // Heading-relative character input (third-person mode): normalised, or
    // zero when no key is held.
    [[nodiscard]] glm::vec3 character_wish(FlyInputGate gate) const;
    // Free-fly movement for this frame.
    void fly(engine::scene::Scene& scene, FlyInputGate gate, float dt);
    // Third-person follow: orbit `target` (the character) at a fixed distance.
    void follow(engine::scene::Scene& scene, entt::entity target);

    [[nodiscard]] glm::vec3 forward() const;
    // View / projection for this camera (Vulkan Y flip, like camera_system).
    [[nodiscard]] engine::scene::CameraMatrices matrices(float aspect) const;

    [[nodiscard]] bool looking() const { return looking_; }

    glm::vec3 pos{0.0F};
    float     yaw = 0.0F;
    float     pitch = 0.0F;
    float     fov_y = 0.0F; // remote camera.set can change it

private:
    float move_speed_ = 3.0F;
    bool  looking_ = false; // right mouse button held -> mouse-look active
    float look_dx_ = 0.0F;  // per-frame accumulated mouse-look delta
    float look_dy_ = 0.0F;
#ifndef ENGINE_EDITOR
    entt::entity camera_entity_ = entt::null;
#endif
};

} // namespace quat::app
