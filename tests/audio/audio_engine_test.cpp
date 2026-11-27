// AudioEngine (miniaudio) without an audio device: the mix is pulled with
// read_frames(), so playback, bus gains and 3D attenuation are measurable.

#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "audio/wav_file.hpp"
#include "engine/audio/audio_engine.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace a = engine::audio;

class AudioEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = a::AudioEngine::create({.no_device = true});
        ASSERT_OK(created);
        engine.emplace(std::move(*created));
        clip = quat::test::write_test_wav("audio_engine_test.wav", 1.0F);
        short_clip = quat::test::write_test_wav("audio_engine_test_short.wav", 0.05F);
        ASSERT_FALSE(clip.empty());
        ASSERT_FALSE(short_clip.empty());
    }

    // Mixes `seconds` of audio and returns the mean absolute level of the
    // last half (spatial / volume gains are smoothed over the first frames).
    [[nodiscard]] float level(float seconds = 0.1F) {
        const auto frames = static_cast<std::size_t>(seconds * 48000.0F);
        std::vector<float> out(frames * engine->channels());
        EXPECT_OK(engine->read_frames(out));
        double sum = 0.0;
        for (std::size_t i = out.size() / 2; i < out.size(); ++i) sum += std::abs(out[i]);
        return static_cast<float>(sum / static_cast<double>(out.size() - out.size() / 2));
    }

    [[nodiscard]] a::SoundHandle load(const std::string& path, bool spatial = false,
                                      a::AudioBus bus = a::AudioBus::sfx) {
        auto sound = engine->load({.path = path, .bus = bus, .spatial = spatial});
        EXPECT_OK(sound);
        return sound ? *sound : a::invalid_sound;
    }

    std::optional<a::AudioEngine> engine;
    std::string                   clip;
    std::string                   short_clip;
};

TEST_F(AudioEngineTest, CreatesWithoutADevice) {
    EXPECT_FALSE(engine->has_device());
    EXPECT_EQ(engine->sample_rate(), 48000U);
    EXPECT_EQ(engine->channels(), 2U);
    EXPECT_EQ(engine->sound_count(), 0U);
    EXPECT_FLOAT_EQ(level(), 0.0F); // nothing playing: silence
}

TEST_F(AudioEngineTest, BusAndMasterVolumesScaleTheMix) {
    engine->set_bus_volume(a::AudioBus::music, 0.25F);
    EXPECT_FLOAT_EQ(engine->bus_volume(a::AudioBus::music), 0.25F);
    engine->set_bus_volume(a::AudioBus::ui, -3.0F); // clamped, never negative
    EXPECT_FLOAT_EQ(engine->bus_volume(a::AudioBus::ui), 0.0F);
    engine->set_master_volume(0.8F);
    EXPECT_FLOAT_EQ(engine->bus_volume(a::AudioBus::master), 0.8F);
    engine->set_master_volume(1.0F);

    const a::SoundHandle s = load(clip);
    engine->set_looping(s, true);
    engine->play(s);
    const float full = level();
    ASSERT_GT(full, 0.1F);

    engine->set_bus_volume(a::AudioBus::sfx, 0.5F);
    EXPECT_NEAR(level() / full, 0.5F, 0.02F);
    engine->set_master_volume(0.5F);
    EXPECT_NEAR(level() / full, 0.25F, 0.02F);
    engine->set_volume(s, 0.5F);
    EXPECT_NEAR(level() / full, 0.125F, 0.02F);
}

TEST_F(AudioEngineTest, LoadsAWavAndControlsPlayback) {
    const a::SoundHandle s = load(clip);
    ASSERT_TRUE(engine->valid(s));
    EXPECT_EQ(engine->sound_count(), 1U);
    EXPECT_EQ(engine->state(s), a::Playback::stopped);
    EXPECT_FLOAT_EQ(level(), 0.0F);

    engine->play(s);
    EXPECT_EQ(engine->state(s), a::Playback::playing);
    EXPECT_GT(level(), 0.1F);

    engine->pause(s);
    EXPECT_EQ(engine->state(s), a::Playback::paused);
    EXPECT_FLOAT_EQ(level(), 0.0F);
    engine->resume(s);
    EXPECT_EQ(engine->state(s), a::Playback::playing);
    EXPECT_GT(level(), 0.1F);

    engine->stop(s);
    EXPECT_EQ(engine->state(s), a::Playback::stopped);
    EXPECT_FLOAT_EQ(level(), 0.0F);

    engine->destroy(s);
    EXPECT_FALSE(engine->valid(s));
    EXPECT_EQ(engine->sound_count(), 0U);
    engine->play(s); // stale handles are ignored
    EXPECT_EQ(engine->state(s), a::Playback::stopped);
}

TEST_F(AudioEngineTest, StreamsMusic) {
    auto s = engine->load({.path = clip, .bus = a::AudioBus::music, .stream = true});
    ASSERT_OK(s);
    engine->play(*s);
    EXPECT_GT(level(), 0.1F);
}

TEST_F(AudioEngineTest, OneShotEndsAndLoopingKeepsPlaying) {
    const a::SoundHandle once = load(short_clip);
    const a::SoundHandle looped = load(short_clip);
    engine->set_looping(looped, true);
    engine->play(once);
    engine->play(looped);
    (void)level(0.2F); // 4x the clip length
    EXPECT_EQ(engine->state(once), a::Playback::stopped);
    EXPECT_EQ(engine->state(looped), a::Playback::playing);

    engine->play(once); // replays from the start after ending
    EXPECT_EQ(engine->state(once), a::Playback::playing);
}

TEST_F(AudioEngineTest, SpatialAttenuationFallsWithDistance) {
    engine->set_listener({.position = glm::vec3(0.0F), .forward = {0.0F, 0.0F, -1.0F}});
    const a::SoundHandle s = load(clip, /*spatial=*/true);
    engine->set_spatial(s, {.min_distance = 1.0F, .max_distance = 50.0F, .rolloff = 1.0F});
    engine->set_looping(s, true);
    engine->play(s);

    const auto level_at = [&](float distance) {
        engine->set_position(s, {0.0F, 0.0F, -distance}); // straight ahead: no panning bias
        (void)level(0.05F);                                 // settle the gain smoothing
        return level();
    };
    const float at_1 = level_at(1.0F);
    const float at_2 = level_at(2.0F);
    const float at_10 = level_at(10.0F);
    const float at_50 = level_at(50.0F);
    const float at_200 = level_at(200.0F);
    ASSERT_GT(at_1, 0.0F);
    // Inverse distance (clamped): gain = min / (min + rolloff * (d - min)).
    EXPECT_NEAR(at_2 / at_1, 0.5F, 0.03F);
    EXPECT_NEAR(at_10 / at_1, 0.1F, 0.01F);
    EXPECT_LT(at_50, at_10);
    EXPECT_NEAR(at_200, at_50, at_50 * 0.05F) << "attenuation must clamp at max_distance";

    // Non-spatial sounds ignore distance entirely.
    const a::SoundHandle flat = load(clip);
    engine->destroy(s);
    engine->set_looping(flat, true);
    engine->play(flat);
    engine->set_position(flat, {0.0F, 0.0F, -1.0F});
    (void)level(0.05F);
    const float flat_near = level();
    engine->set_position(flat, {0.0F, 0.0F, -100.0F});
    (void)level(0.05F);
    EXPECT_GT(flat_near, 0.1F);
    EXPECT_FLOAT_EQ(level(), flat_near);
}

TEST_F(AudioEngineTest, HandlesAreGenerationChecked) {
    const a::SoundHandle first = load(clip);
    engine->destroy(first);
    const a::SoundHandle second = load(clip); // reuses the slot
    EXPECT_NE(first, second);
    EXPECT_FALSE(engine->valid(first));
    EXPECT_TRUE(engine->valid(second));
    engine->destroy(first); // must not free `second`
    EXPECT_TRUE(engine->valid(second));
}

TEST_F(AudioEngineTest, OwnedSoundsAreListedForSweeping) {
    auto owned = engine->load({.path = clip, .owner = 7});
    ASSERT_OK(owned);
    (void)load(clip); // unowned: not listed
    const auto list = engine->owned_sounds();
    ASSERT_EQ(list.size(), 1U);
    EXPECT_EQ(list[0].sound, *owned);
    EXPECT_EQ(list[0].owner, 7U);
    EXPECT_EQ(engine->owner(*owned), 7U);
}

TEST_F(AudioEngineTest, RejectsBadInputWithoutAborting) {
    const auto missing = engine->load({.path = clip + ".missing.wav"});
    ASSERT_ERR(missing);
    EXPECT_NE(missing.error().message.find("missing"), std::string::npos)
        << missing.error().message;
    EXPECT_ERR(engine->load({.path = ""}));
    std::vector<float> odd(3); // 1.5 stereo frames
    EXPECT_ERR(engine->read_frames(odd));
}

TEST_F(AudioEngineTest, ResolvesRelativePathsAgainstTheAssetRoot) {
    auto rooted = a::AudioEngine::create({.no_device = true, .asset_root = ::testing::TempDir()});
    ASSERT_OK(rooted);
    EXPECT_OK(rooted->load({.path = "audio_engine_test.wav"}));
}

} // namespace
