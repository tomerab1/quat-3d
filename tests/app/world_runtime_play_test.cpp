// WorldRuntime::apply_play's transition contract (train #93): it reports
// whether the scene switched between edit and play, because the callers reset
// the renderer's temporal history (R11) on exactly those frames. Play and Stop
// are transitions; an unchanged request is not.

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "app/play_mode.hpp"
#include "app/world_runtime.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace {

TEST(WorldRuntimePlay, ApplyPlayReportsOnlyTransitions) {
    engine::scene::Scene scene;
    const entt::entity crate = scene.create_entity("crate");
    scene.registry().emplace<engine::scene::Collider>(crate);
    quat::app::WorldRuntime world;
    quat::app::PlayMode mode;

    EXPECT_FALSE(world.apply_play(mode, scene)) << "nothing requested: no transition";

    *mode.request() = true;
    EXPECT_TRUE(world.apply_play(mode, scene)) << "Play is a transition";
    EXPECT_TRUE(mode.active());
    EXPECT_TRUE(world.play_physics_active());
    EXPECT_FALSE(world.apply_play(mode, scene)) << "still playing: no transition";

    *mode.request() = false;
    EXPECT_TRUE(world.apply_play(mode, scene)) << "Stop is a transition";
    EXPECT_FALSE(mode.active());
    EXPECT_FALSE(world.play_physics_active());
    EXPECT_FALSE(world.apply_play(mode, scene)) << "stopped: no transition";
    EXPECT_FALSE(scene.registry().all_of<engine::scene::RigidBody>(crate))
        << "Stop restored the authored scene (no promoted body)";
}

} // namespace
