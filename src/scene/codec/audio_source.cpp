// Codec component `audio_source`: Sound emitter: wav/flac/mp3 clip on a bus, optionally 3D.
// Registered by the generated engine_components.cpp (ROADMAP S9); see
// include/engine/scene/component_registry.hpp and src/scene/codec/README.md.

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/json_fields.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "scene/component_json_internal.hpp"

namespace engine::scene::codec {

namespace {

using namespace detail;
using core::EnumName;
using core::JsonFields;

constexpr EnumName<audio::AudioBus> k_audio_buses[] = {
    {"master", audio::AudioBus::master}, {"music", audio::AudioBus::music},
    {"sfx", audio::AudioBus::sfx},       {"ui", audio::AudioBus::ui},
    {"ambience", audio::AudioBus::ambience}};
constexpr EnumName<AudioCommand> k_audio_commands[] = {
    {"none", AudioCommand::none}, {"play", AudioCommand::play}, {"stop", AudioCommand::stop},
    {"pause", AudioCommand::pause}, {"resume", AudioCommand::resume}};
constexpr std::size_t k_max_clip_path = 4096;

// Clips are project-relative (issue #66); AudioEngine::load re-checks against
// the real root, symlinks included. Null when `clip` is acceptable.
[[nodiscard]] const char* clip_path_problem(const std::string& clip) {
    if (clip.empty()) return nullptr;
    const std::filesystem::path p = core::path_from_utf8(clip);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) {
        return "must be a path relative to the project root, not an absolute path";
    }
    const std::filesystem::path norm = p.lexically_normal();
    if (!norm.empty() && *norm.begin() == "..") return "must stay inside the project root";
    return nullptr;
}

} // namespace

std::expected<void, core::Error> register_audio_source(ComponentRegistry& registry) {
    ComponentCodec codec{.name = "audio_source",
                         .summary = "Sound emitter: wav/flac/mp3 clip on a bus, optionally 3D",
                         .addable = true,
                         .writable = true,
                         .order = 2300};
    codec.encode = [](const entt::registry& r, entt::entity e, const EncodeOptions& /*options*/) {
        const AudioSource& a = r.get<AudioSource>(e);
        return json({{"clip", a.clip},
                     {"bus", enum_name(k_audio_buses, a.bus)},
                     {"volume", a.volume},
                     {"pitch", a.pitch},
                     {"loop", a.loop},
                     {"play_on_start", a.play_on_start},
                     {"spatial", a.spatial},
                     {"min_distance", a.min_distance},
                     {"max_distance", a.max_distance},
                     {"rolloff", a.rolloff},
                     {"doppler", a.doppler}});
    };
    codec.apply = [](entt::registry& r, entt::entity e, const json& v, bool strict)
        -> std::expected<void, core::Error> {
        constexpr std::string_view name = "audio_source";
        // `command` is write-only: the audio system consumes it (play, stop,
        // pause, resume) — how remote tools trigger a sound.
        return merge<AudioSource>(r, e, v, name, strict, [](JsonFields& f, AudioSource& a) {
            f.string("clip", a.clip, k_max_clip_path).enumeration("bus", a.bus, k_audio_buses);
            if (a.clip.find('\0') != std::string::npos) f.fail("clip", "must not contain NUL");
            if (const char* why = clip_path_problem(a.clip); why != nullptr) f.fail("clip", why);
            f.number("volume", a.volume, 0.0F, 16.0F).number("pitch", a.pitch, 0.01F, 16.0F);
            f.boolean("loop", a.loop).boolean("play_on_start", a.play_on_start);
            f.boolean("spatial", a.spatial);
            f.number("min_distance", a.min_distance, 0.01F, 1.0e6F);
            f.number("max_distance", a.max_distance, 0.01F, 1.0e6F);
            f.number("rolloff", a.rolloff, 0.0F, 100.0F).number("doppler", a.doppler, 0.0F, 10.0F);
            f.enumeration("command", a.command, k_audio_commands);
            if (a.max_distance < a.min_distance) f.fail("max_distance", "must be >= min_distance");
        });
    };
    return registry.add(typed_codec<AudioSource>(std::move(codec)));
}

} // namespace engine::scene::codec
