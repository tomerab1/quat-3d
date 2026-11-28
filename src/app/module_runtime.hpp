#pragma once

// ModuleRuntime — runs a game's native module systems (ROADMAP G17.1) in
// their phases of the frame. WorldRuntime::simulate calls it; it runs only in
// play (the player is always in play; the editor during Play), since module
// systems are gameplay.

#include <cstdint>
#include <filesystem>

#include "engine/game/module_set.hpp"
#include "engine/scene/scene.hpp"

namespace engine::input {
class Actions;
}
namespace engine::physics {
class PhysicsWorld;
}

namespace quat::app {

class ModuleRuntime {
public:
    // `modules` (null: none) outlives this; `actions` are the game's G5 actions.
    void bind(const engine::game::ModuleSet* modules, engine::input::Actions* actions,
              std::filesystem::path project_root);
    [[nodiscard]] const engine::game::ModuleSet* modules() const { return modules_; }
    [[nodiscard]] bool active() const { return modules_ != nullptr && !modules_->empty(); }

    // Runs `phase`'s systems; `physics` is the running world (may be null).
    void run(engine::game::Phase phase, engine::scene::Scene& scene,
             engine::physics::PhysicsWorld* physics, std::uint64_t frame, float dt) const;
    // Scene::tick hooks running post_transform and pre_render.
    [[nodiscard]] engine::scene::SceneTickHooks hooks(engine::scene::Scene& scene,
                                                      engine::physics::PhysicsWorld* physics,
                                                      std::uint64_t frame) const;
    // The services a method handler or system sees.
    [[nodiscard]] engine::game::Services services(engine::physics::PhysicsWorld* physics) const;

private:
    const engine::game::ModuleSet* modules_ = nullptr;
    engine::input::Actions*        actions_ = nullptr;
    std::filesystem::path          project_root_;
};

} // namespace quat::app
