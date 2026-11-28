#pragma once

// InputRuntime — the application's input actions (G5.1) and camera rigs
// (G15.1), window-free so tests drive the same path (game_world library).
//
// The action map is the engine default (G7's names) merged with the
// project's `assets/input/actions.json` when present; the user's rebinding
// file (user_overrides_path) is applied on top and saved back by
// save_overrides(). Each frame: update() evaluates the actions from the
// sampled device state, then update_rigs() runs camera_rig_system — with
// input only while the game view is live (play through the scene camera),
// so the editor's fly camera and a rig never both take WASD.

#include <filesystem>
#include <optional>

#include <glm/glm.hpp>

#include "engine/core/error.hpp"
#include "engine/input/actions.hpp"
#include "engine/input/device_state.hpp"
#include "engine/scene/navigation.hpp" // GroundHeightFn

namespace engine::physics {
class PhysicsWorld;
}
namespace engine::scene {
class Scene;
}

namespace quat::app {

class InputRuntime {
public:
    // Loads the project map and the user's overrides; a bad file is logged
    // and skipped (the defaults stay usable).
    void init(const std::filesystem::path& project_root,
              std::optional<std::filesystem::path> user_overrides_path);

    [[nodiscard]] engine::input::Actions& actions() { return actions_; }
    [[nodiscard]] const engine::input::Actions& actions() const { return actions_; }
    [[nodiscard]] const std::optional<std::filesystem::path>& user_overrides_path() const {
        return user_path_;
    }
    [[nodiscard]] std::expected<void, engine::core::Error> save_overrides() const;

    // Once per frame after sampling; `camera_yaw_rad` steers camera_relative
    // modifiers (the view's heading).
    void update(const engine::input::DeviceState& device, float dt, float camera_yaw_rad);

    // Camera rigs: springs always, input only when `rig_input` is set.
    // `physics` (play world, may be null) gives the pointer's ground point.
    void update_rigs(engine::scene::Scene& scene, const engine::scene::GroundHeightFn& ground,
                     const engine::physics::PhysicsWorld* physics, bool rig_input, float dt);

private:
    engine::input::Actions                 actions_;
    engine::input::DeviceState             device_; // this frame's, for the rig pointer
    std::optional<std::filesystem::path>   user_path_;
};

} // namespace quat::app
