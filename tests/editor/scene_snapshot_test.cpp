// Play-mode snapshot (capture on Play, restore on Stop).

#ifdef ENGINE_EDITOR

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "editor/scene_snapshot.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace {

namespace s = engine::scene;

// Audit B12/C9: Stop used to drop NetReplicated (replication silently ended
// after one Play/Stop cycle); Primitive must survive the same way.
TEST(PlayModeSnapshot, RestoresNetReplicatedAndPrimitive) {
    s::Scene scene;
    const entt::entity e = scene.create_entity("crate");
    scene.registry().emplace<s::NetReplicated>(e, 7U);
    scene.registry().emplace<s::Primitive>(e).shape = s::PrimitiveShape::cone;

    const auto snapshot = engine::editor::capture_scene(scene);
    scene.registry().remove<s::NetReplicated, s::Primitive>(e);
    engine::editor::restore_scene(scene, snapshot);

    ASSERT_TRUE(scene.registry().valid(e));
    ASSERT_TRUE(scene.registry().all_of<s::NetReplicated>(e));
    EXPECT_EQ(scene.registry().get<s::NetReplicated>(e).net_id, 7U);
    ASSERT_TRUE(scene.registry().all_of<s::Primitive>(e));
    EXPECT_EQ(scene.registry().get<s::Primitive>(e).shape, s::PrimitiveShape::cone);
}

// Audio components survive Play/Stop with their authored fields; the play
// session's sound handle and playback state do not (the audio system's sweep
// frees the sound, and play_on_start fires again on the next Play).
TEST(PlayModeSnapshot, RestoresAudioComponentsWithoutRuntimeState) {
    s::Scene scene;
    const entt::entity e = scene.create_entity("radio");
    s::AudioSource src;
    src.clip = "assets/audio/song.mp3";
    src.bus = engine::audio::AudioBus::music;
    src.volume = 0.5F;
    scene.registry().emplace<s::AudioSource>(e, src);
    scene.registry().emplace<s::AudioListener>(e).active = false;

    const auto snapshot = engine::editor::capture_scene(scene);
    auto& live = scene.registry().get<s::AudioSource>(e);
    live.sound = 42;
    live.started = true;
    live.state = engine::audio::Playback::playing;
    live.volume = 1.0F;
    engine::editor::restore_scene(scene, snapshot);

    ASSERT_TRUE(scene.registry().all_of<s::AudioSource>(e));
    const auto& restored = scene.registry().get<s::AudioSource>(e);
    EXPECT_EQ(restored.clip, "assets/audio/song.mp3");
    EXPECT_EQ(restored.bus, engine::audio::AudioBus::music);
    EXPECT_FLOAT_EQ(restored.volume, 0.5F);
    EXPECT_EQ(restored.sound, engine::audio::invalid_sound);
    EXPECT_FALSE(restored.started);
    EXPECT_EQ(restored.state, engine::audio::Playback::stopped);
    ASSERT_TRUE(scene.registry().all_of<s::AudioListener>(e));
    EXPECT_FALSE(scene.registry().get<s::AudioListener>(e).active);
}

} // namespace

#endif // ENGINE_EDITOR
