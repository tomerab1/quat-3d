// audio_system: AudioSource / AudioListener components driving a no-device
// AudioEngine.

#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "audio/wav_file.hpp"
#include "engine/audio/audio_engine.hpp"
#include "engine/scene/audio_system.hpp"
#include "engine/scene/components.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace a = engine::audio;
namespace s = engine::scene;

class AudioSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = a::AudioEngine::create({.no_device = true});
        ASSERT_OK(created);
        engine.emplace(std::move(*created));
        clip = quat::test::write_test_wav("audio_system_test.wav", 1.0F);
        other_clip = quat::test::write_test_wav("audio_system_test_other.wav", 0.5F);
        ASSERT_FALSE(clip.empty());
        ASSERT_FALSE(other_clip.empty());
    }

    entt::entity make_source(const glm::vec3& position = glm::vec3(0.0F)) {
        const entt::entity e = r.create();
        s::Transform t;
        t.world = glm::translate(glm::mat4(1.0F), position);
        t.local = t.world;
        r.emplace<s::Transform>(e, t);
        s::AudioSource src;
        src.clip = clip;
        src.loop = true;
        r.emplace<s::AudioSource>(e, src);
        return e;
    }

    void tick(bool simulate, float dt = 1.0F / 60.0F) {
        s::audio_system(r, *engine, dt, {.simulate = simulate, .camera_world = camera});
    }

    [[nodiscard]] float level(float seconds = 0.1F) {
        std::vector<float> out(static_cast<std::size_t>(seconds * 48000.0F) * engine->channels());
        EXPECT_OK(engine->read_frames(out));
        double sum = 0.0;
        for (std::size_t i = out.size() / 2; i < out.size(); ++i) sum += std::abs(out[i]);
        return static_cast<float>(sum / static_cast<double>(out.size() - out.size() / 2));
    }

    entt::registry                r;
    std::optional<a::AudioEngine> engine;
    std::optional<glm::mat4>      camera;
    std::string                   clip;
    std::string                   other_clip;
};

TEST_F(AudioSystemTest, PlayOnStartPlaysInPlayModeOnly) {
    const entt::entity e = make_source();
    tick(/*simulate=*/false); // edit mode: nothing loads, nothing plays
    EXPECT_EQ(engine->sound_count(), 0U);
    EXPECT_EQ(r.get<s::AudioSource>(e).state, a::Playback::stopped);

    tick(/*simulate=*/true);
    const auto& src = r.get<s::AudioSource>(e);
    ASSERT_TRUE(engine->valid(src.sound));
    EXPECT_EQ(src.state, a::Playback::playing);
    EXPECT_TRUE(src.started);
    EXPECT_GT(level(), 0.1F);

    // play_on_start fires once: a later stop is not undone next frame.
    r.get<s::AudioSource>(e).command = s::AudioCommand::stop;
    tick(true);
    tick(true);
    EXPECT_EQ(r.get<s::AudioSource>(e).state, a::Playback::stopped);
}

TEST_F(AudioSystemTest, SourcesWithoutPlayOnStartWaitForACommand) {
    const entt::entity e = make_source();
    r.get<s::AudioSource>(e).play_on_start = false;
    tick(true);
    auto& src = r.get<s::AudioSource>(e);
    EXPECT_TRUE(engine->valid(src.sound)) << "play mode preloads every source";
    EXPECT_EQ(src.state, a::Playback::stopped);

    src.command = s::AudioCommand::play;
    tick(true);
    EXPECT_EQ(src.command, s::AudioCommand::none) << "commands are consumed";
    EXPECT_EQ(src.state, a::Playback::playing);
    src.command = s::AudioCommand::pause;
    tick(true);
    EXPECT_EQ(src.state, a::Playback::paused);
    src.command = s::AudioCommand::resume;
    tick(true);
    EXPECT_EQ(src.state, a::Playback::playing);
}

TEST_F(AudioSystemTest, EditModePreviewPlaysOnCommand) {
    const entt::entity e = make_source();
    r.get<s::AudioSource>(e).command = s::AudioCommand::play;
    tick(/*simulate=*/false);
    const auto& src = r.get<s::AudioSource>(e);
    EXPECT_EQ(src.state, a::Playback::playing);
    EXPECT_FALSE(src.started) << "edit mode must not consume play_on_start";
}

TEST_F(AudioSystemTest, ReleasesSoundsOfDestroyedEntitiesAndRemovedComponents) {
    const entt::entity doomed = make_source();
    const entt::entity stripped = make_source();
    const entt::entity kept = make_source();
    tick(true);
    ASSERT_EQ(engine->sound_count(), 3U);

    r.destroy(doomed);
    r.remove<s::AudioSource>(stripped);
    tick(true);
    EXPECT_EQ(engine->sound_count(), 1U);
    EXPECT_TRUE(engine->valid(r.get<s::AudioSource>(kept).sound));
}

TEST_F(AudioSystemTest, ClipChangeReloadsAndCopiesGetTheirOwnSound) {
    const entt::entity e = make_source();
    tick(true);
    const a::SoundHandle first = r.get<s::AudioSource>(e).sound;

    r.get<s::AudioSource>(e).clip = other_clip;
    tick(true);
    const a::SoundHandle second = r.get<s::AudioSource>(e).sound;
    EXPECT_FALSE(engine->valid(first));
    EXPECT_TRUE(engine->valid(second));
    EXPECT_EQ(engine->sound_count(), 1U);

    // A duplicated entity copies the component, handle included.
    const entt::entity copy = r.create();
    r.emplace<s::Transform>(copy);
    r.emplace<s::AudioSource>(copy, r.get<s::AudioSource>(e));
    tick(true);
    EXPECT_NE(r.get<s::AudioSource>(copy).sound, second);
    EXPECT_TRUE(engine->valid(r.get<s::AudioSource>(e).sound));
    EXPECT_TRUE(engine->valid(r.get<s::AudioSource>(copy).sound));
    EXPECT_EQ(engine->sound_count(), 2U);
}

TEST_F(AudioSystemTest, MissingClipFailsWithoutRetryingEveryFrame) {
    const entt::entity e = make_source();
    r.get<s::AudioSource>(e).clip = clip + ".missing.wav";
    tick(true);
    tick(true);
    const auto& src = r.get<s::AudioSource>(e);
    EXPECT_EQ(src.sound, a::invalid_sound);
    EXPECT_NE(src.resolved, 0U) << "the failed attempt is remembered";
    EXPECT_EQ(src.state, a::Playback::stopped);
    EXPECT_EQ(engine->sound_count(), 0U);
}

TEST_F(AudioSystemTest, SpatialSourcesFollowTheirTransform) {
    const entt::entity e = make_source({0.0F, 0.0F, -1.0F}); // listener at origin, facing -Z
    auto& src = r.get<s::AudioSource>(e);
    src.min_distance = 1.0F;
    src.max_distance = 100.0F;
    tick(true);
    (void)level(0.05F);
    const float near = level();

    r.get<s::Transform>(e).world = glm::translate(glm::mat4(1.0F), {0.0F, 0.0F, -10.0F});
    tick(true);
    (void)level(0.05F);
    EXPECT_NEAR(level() / near, 0.1F, 0.01F);
}

TEST_F(AudioSystemTest, ListenerFollowsTheActiveListenerElseTheCamera) {
    camera = glm::translate(glm::mat4(1.0F), {0.0F, 2.0F, 0.0F});
    tick(true);
    EXPECT_EQ(engine->listener().position, glm::vec3(0.0F, 2.0F, 0.0F));

    const entt::entity ears = r.create();
    s::Transform t;
    t.world = glm::translate(glm::mat4(1.0F), {5.0F, 0.0F, 0.0F});
    r.emplace<s::Transform>(ears, t);
    r.emplace<s::AudioListener>(ears);
    tick(true);
    EXPECT_EQ(engine->listener().position, glm::vec3(5.0F, 0.0F, 0.0F));
    EXPECT_EQ(engine->listener().forward, glm::vec3(0.0F, 0.0F, -1.0F));

    r.get<s::AudioListener>(ears).active = false;
    tick(true);
    EXPECT_EQ(engine->listener().position, glm::vec3(0.0F, 2.0F, 0.0F));
}

} // namespace
