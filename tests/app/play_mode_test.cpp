// G7 review: the Play/Stop state machine the editor and the non-editor game
// share (src/app/play_mode). Play snapshots and starts the session, Stop ends
// it and restores the authored scene; a Play that fails leaves no trace.

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "app/play_mode.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace {

using engine::scene::Collider;
using engine::scene::RigidBody;
using quat::app::PlayMode;

TEST(PlayMode, PlayThenStopRestoresTheAuthoredScene) {
    engine::scene::Scene scene;
    const entt::entity wall = scene.create_entity("wall");
    scene.registry().emplace<Collider>(wall);
    int begins = 0;
    int ends = 0;
    const PlayMode::Hooks hooks{
        .begin = [&](engine::scene::Scene&) -> std::expected<void, engine::core::Error> {
            ++begins;
            return {};
        },
        .end = [&] { ++ends; }};

    PlayMode mode;
    EXPECT_TRUE(mode.apply(scene, hooks));
    EXPECT_EQ(begins, 0) << "nothing requested";

    *mode.request() = true;
    EXPECT_TRUE(mode.apply(scene, hooks));
    EXPECT_TRUE(mode.active());
    EXPECT_EQ(begins, 1);
    EXPECT_TRUE(scene.registry().all_of<RigidBody>(wall)) << "collider-only bodies are promoted";
    const entt::entity spawned = scene.create_entity("spawned during play");
    EXPECT_TRUE(mode.apply(scene, hooks));
    EXPECT_EQ(begins, 1) << "no transition while the request is unchanged";

    *mode.request() = false;
    EXPECT_TRUE(mode.apply(scene, hooks));
    EXPECT_FALSE(mode.active());
    EXPECT_EQ(ends, 1);
    EXPECT_FALSE(scene.registry().valid(spawned));
    EXPECT_TRUE(scene.registry().valid(wall));
    EXPECT_FALSE(scene.registry().all_of<RigidBody>(wall)) << "the promotion is undone";
}

TEST(PlayMode, AFailedPlayClearsTheRequestAndTheScene) {
    engine::scene::Scene scene;
    const entt::entity wall = scene.create_entity("wall");
    scene.registry().emplace<Collider>(wall);
    PlayMode mode;
    *mode.request() = true;
    const PlayMode::Hooks failing{
        .begin = [](engine::scene::Scene&) -> std::expected<void, engine::core::Error> {
            return std::unexpected(engine::core::Error{"no physics"});
        },
        .end = [] { FAIL() << "end without a session"; }};
    EXPECT_FALSE(mode.apply(scene, failing));
    EXPECT_FALSE(mode.requested());
    EXPECT_FALSE(mode.active());
    EXPECT_FALSE(scene.registry().all_of<RigidBody>(wall));
    EXPECT_TRUE(mode.apply(scene, failing)) << "a cleared request is no transition";
}

} // namespace
