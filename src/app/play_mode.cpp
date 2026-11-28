#include "app/play_mode.hpp"

#include <cstdio>

#include <entt/entity/registry.hpp>

#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace quat::app {

bool PlayMode::apply(engine::scene::Scene& scene, const Hooks& hooks) {
    if (requested_ == active_) return true;
    if (requested_) {
        snapshot_ = engine::scene::capture_scene(scene);
        // Unity convention: a Collider without a RigidBody simulates as a
        // static body (kinematic on nav agents, none on characters). Added
        // AFTER the snapshot, so Stop removes it.
        (void)engine::scene::promote_collider_only_bodies(scene.registry());
        auto started = hooks.begin ? hooks.begin(scene) : std::expected<void, engine::core::Error>{};
        if (!started) {
            std::fprintf(stderr, "[play] play failed: %s\n", started.error().message.c_str());
            engine::scene::restore_scene(scene, *snapshot_); // drop the promoted bodies
            snapshot_.reset();
            requested_ = false;
            return false;
        }
        std::fprintf(stderr, "[play] play (%zu colliders, %zu bodies)\n",
                     scene.registry().view<engine::scene::Collider>().size(),
                     scene.registry().view<engine::scene::RigidBody>().size());
        active_ = true;
        return true;
    }
    if (hooks.end) hooks.end();
    if (snapshot_) {
        engine::scene::restore_scene(scene, *snapshot_);
        snapshot_.reset();
    }
    std::fprintf(stderr, "[play] stop\n");
    active_ = false;
    return true;
}

} // namespace quat::app
