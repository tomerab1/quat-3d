// ModuleRuntime — see module_runtime.hpp.

#include "app/module_runtime.hpp"

#include <utility>

#include "engine/core/profile.hpp"

namespace quat::app {

void ModuleRuntime::bind(const engine::game::ModuleSet* modules, engine::input::Actions* actions,
                         std::filesystem::path project_root) {
    modules_ = modules;
    actions_ = actions;
    project_root_ = std::move(project_root);
}

engine::game::Services ModuleRuntime::services(engine::physics::PhysicsWorld* physics) const {
    return engine::game::Services{.physics = physics, .actions = actions_, .project_root = project_root_};
}

void ModuleRuntime::run(engine::game::Phase phase, engine::scene::Scene& scene,
                        engine::physics::PhysicsWorld* physics, std::uint64_t frame, float dt) const {
    if (!active() || modules_->systems(phase).empty()) return;
    QUAT_ZONE_C("module_phase", engine::core::ProfileCategory::system);
    engine::game::Services s = services(physics);
    engine::game::SystemContext context(scene, s, frame);
    modules_->run(phase, context, dt);
}

engine::scene::SceneTickHooks ModuleRuntime::hooks(engine::scene::Scene& scene,
                                                   engine::physics::PhysicsWorld* physics,
                                                   std::uint64_t frame) const {
    if (!active()) return {};
    return {.post_transform =
                [this, &scene, physics, frame](float dt) {
                    run(engine::game::Phase::post_transform, scene, physics, frame, dt);
                },
            .pre_render =
                [this, &scene, physics, frame](float dt) {
                    run(engine::game::Phase::pre_render, scene, physics, frame, dt);
                }};
}

} // namespace quat::app
