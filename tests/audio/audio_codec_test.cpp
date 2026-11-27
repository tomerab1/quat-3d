// JSON codec entries for the audio components (scene files + remote API).

#include <string>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "support/expect_ok.hpp"

namespace {

using json = nlohmann::json;
namespace s = engine::scene;
namespace a = engine::audio;

class AudioCodec : public ::testing::Test {
protected:
    entt::registry r;
    entt::entity   e = r.create();

    [[nodiscard]] auto apply(std::string_view name, const json& v, bool strict = true) {
        return s::apply_component(r, e, name, v, strict);
    }
};

TEST_F(AudioCodec, RoundTripsSourceAndListener) {
    const json authored = {
        {"audio_source", {{"clip", "assets/audio/wind.flac"}, {"bus", "ambience"}, {"volume", 0.75},
                          {"pitch", 1.25}, {"loop", true}, {"play_on_start", false},
                          {"spatial", true}, {"min_distance", 2}, {"max_distance", 40},
                          {"rolloff", 0.5}, {"doppler", 1}}},
        {"audio_listener", {{"active", false}}},
    };
    for (const auto& [name, value] : authored.items()) ASSERT_OK(apply(name, value)) << name;

    const auto& src = r.get<s::AudioSource>(e);
    EXPECT_EQ(src.clip, "assets/audio/wind.flac");
    EXPECT_EQ(src.bus, a::AudioBus::ambience);
    EXPECT_FLOAT_EQ(src.pitch, 1.25F);
    EXPECT_FALSE(src.play_on_start);
    EXPECT_FALSE(r.get<s::AudioListener>(e).active);

    const json first = s::encode_components(r, e);
    EXPECT_EQ(first["audio_source"], authored["audio_source"]);
    const entt::entity copy = r.create();
    for (const auto& [name, value] : first.items()) {
        ASSERT_OK(s::apply_component(r, copy, name, value, true)) << name;
    }
    EXPECT_EQ(s::encode_components(r, copy), first);
}

TEST_F(AudioCodec, CommandIsWriteOnlyAndMergesKeepOtherFields) {
    ASSERT_OK(apply("audio_source", {{"clip", "a.wav"}, {"volume", 0.3}}));
    ASSERT_OK(apply("audio_source", {{"command", "play"}}));
    const auto& src = r.get<s::AudioSource>(e);
    EXPECT_EQ(src.command, s::AudioCommand::play);
    EXPECT_EQ(src.clip, "a.wav");
    EXPECT_FLOAT_EQ(src.volume, 0.3F);
    EXPECT_FALSE(s::encode_components(r, e)["audio_source"].contains("command"));
}

TEST_F(AudioCodec, RejectsMalformedValues) {
    const json cases[] = {
        {{"clip", 5}},
        {{"clip", std::string("a\0b.wav", 7)}},
        {{"clip", std::string(5000, 'x')}},
        {{"bus", "loud"}},
        {{"volume", -1}},
        {{"volume", "max"}},
        {{"pitch", 0}},
        {{"min_distance", 0}},
        {{"min_distance", 10}, {"max_distance", 5}},
        {{"rolloff", -1}},
        {{"doppler", 100}},
        {{"loop", 1}},
        {{"command", "rewind"}},
        {{"colume", 1}},
        json::array({1, 2}),
    };
    for (const json& c : cases) {
        EXPECT_FALSE(apply("audio_source", c)) << "accepted " << c.dump();
    }
    EXPECT_FALSE(apply("audio_listener", {{"active", "yes"}}));
    EXPECT_FALSE(r.all_of<s::AudioSource>(e)) << "a rejected decode must not add the component";
    // Scene files decode leniently: unknown fields are skipped.
    EXPECT_OK(apply("audio_source", {{"colume", 1}}, /*strict=*/false));
}

TEST_F(AudioCodec, RemovesComponents) {
    ASSERT_OK(apply("audio_source", json::object()));
    ASSERT_OK(apply("audio_listener", json::object()));
    EXPECT_TRUE(s::remove_component(r, e, "audio_source"));
    EXPECT_TRUE(s::remove_component(r, e, "audio_listener"));
    EXPECT_FALSE((r.any_of<s::AudioSource, s::AudioListener>(e)));
    const auto* info = s::find_component_info("audio_source");
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->addable);
}

} // namespace
