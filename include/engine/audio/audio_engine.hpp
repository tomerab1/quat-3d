#pragma once

// AudioEngine — miniaudio-backed mixer and 3D audio (ROADMAP G1).
//
// Wraps an ma_engine plus one ma_sound_group per bus (master -> music, sfx,
// ui, ambience) and a slot table of sounds addressed by generation-checked
// handles. Sounds load from wav / flac / mp3 files (decoded by miniaudio),
// either fully decoded into shared memory or streamed from disk. Spatial
// sounds attenuate with the inverse-distance model between min and max
// distance (with rolloff) and pan relative to the single listener; doppler is
// off unless a sound opts in. miniaudio types never appear here (PIMPL).
//
// Threading: every call is main-thread only (the mixer itself runs on the
// device's audio thread; miniaudio synchronises parameter changes with it).
//
// `no_device` engines open no playback device: nothing is heard, and the mix
// advances only when read_frames() pulls it — used by tests and headless runs.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/audio/audio_types.hpp"
#include "engine/core/error.hpp"

namespace engine::audio {

struct AudioEngineConfig {
    bool          no_device = false;
    std::uint32_t sample_rate = 0; // 0: the device's native rate (48 kHz without a device)
    std::uint32_t channels = 0;    // 0: the device's native layout (stereo without a device)
    // Clip paths are relative to this directory (empty: the CWD) and must
    // stay inside it (issue #66: clips come from scene files and remote
    // requests).
    std::filesystem::path asset_root{};
    // Most memory a non-streamed sound may decode into: its length at the
    // engine's sample rate × channels × sizeof(float), read from the file's
    // headers before anything is decoded (a file larger than this is refused
    // without being opened). A clip of unknown length can only be streamed.
    // Streamed sounds read incrementally and have no cap.
    std::uint64_t max_decoded_clip_bytes = 64ULL << 20U;
};

// How a sound is created. Everything else (volume, pitch, looping, spatial
// parameters, position) can change at any time after load().
struct SoundDesc {
    std::string   path{};
    AudioBus      bus = AudioBus::sfx;
    bool          stream = false;  // stream from disk instead of decoding up front
    bool          spatial = false; // 3D: attenuate + pan relative to the listener
    std::uint32_t owner = no_owner;
};

// 3D parameters of a spatial sound (ignored by non-spatial ones).
struct SpatialParams {
    float min_distance = 1.0F;   // full volume inside this radius
    float max_distance = 100.0F; // attenuation stops (clamps) beyond this
    float rolloff = 1.0F;        // inverse-distance rolloff factor
    float doppler = 0.0F;        // doppler factor; 0 = off (the default)
};

// The listener's world pose. `forward` / `up` need not be normalised.
struct ListenerPose {
    glm::vec3 position{0.0F};
    glm::vec3 forward{0.0F, 0.0F, -1.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    glm::vec3 velocity{0.0F}; // for doppler
};

class AudioEngine {
public:
    [[nodiscard]] static std::expected<AudioEngine, core::Error>
    create(const AudioEngineConfig& config = {});

    AudioEngine(AudioEngine&&) noexcept;
    AudioEngine& operator=(AudioEngine&&) noexcept;
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    ~AudioEngine();

    [[nodiscard]] bool          has_device() const;
    [[nodiscard]] std::uint32_t sample_rate() const;
    [[nodiscard]] std::uint32_t channels() const;

    // ---- Mixer -------------------------------------------------------------
    // Linear gains (1 = unity). The master volume is the `master` bus.
    void                set_master_volume(float volume);
    [[nodiscard]] float master_volume() const;
    void                set_bus_volume(AudioBus bus, float volume);
    [[nodiscard]] float bus_volume(AudioBus bus) const;

    // ---- Sounds ------------------------------------------------------------
    // Load a sound (stopped, cursor at the start). The path is confined to
    // the asset root (core::confine_to_root: no absolute paths, no `..` or
    // symlink escapes) and must name a regular file — never a FIFO or device,
    // which would block the main thread — whose decoded size is within
    // max_decoded_clip_bytes unless streamed. Errors name the clip as given
    // (never the resolved absolute path) and the reason (outside the root,
    // not a file, too large, missing, unsupported format, ...).
    [[nodiscard]] std::expected<SoundHandle, core::Error> load(const SoundDesc& desc);
    void destroy(SoundHandle sound);
    // Stops and destroys every sound (the buses and their volumes remain).
    void destroy_all();

    [[nodiscard]] bool          valid(SoundHandle sound) const;
    [[nodiscard]] std::uint32_t owner(SoundHandle sound) const; // no_owner if invalid
    [[nodiscard]] std::size_t   sound_count() const;

    // (sound, owner) for every live sound created with an owner, so the owner
    // can sweep away sounds whose owner no longer exists.
    struct OwnedSound {
        SoundHandle   sound = invalid_sound;
        std::uint32_t owner = no_owner;
    };
    [[nodiscard]] std::vector<OwnedSound> owned_sounds() const;

    // Transport. play() restarts from the beginning; pause() keeps the
    // cursor for resume(); stop() rewinds. Invalid handles are ignored.
    void play(SoundHandle sound);
    void stop(SoundHandle sound);
    void pause(SoundHandle sound);
    void resume(SoundHandle sound);
    void stop_all();
    // A non-looping sound that played to its end reports `stopped`.
    [[nodiscard]] Playback state(SoundHandle sound) const;

    void set_volume(SoundHandle sound, float volume);
    void set_pitch(SoundHandle sound, float pitch); // playback-rate multiplier (> 0)
    void set_looping(SoundHandle sound, bool looping);
    void set_spatial(SoundHandle sound, const SpatialParams& params);
    void set_position(SoundHandle sound, const glm::vec3& position);
    void set_velocity(SoundHandle sound, const glm::vec3& velocity);

    void set_listener(const ListenerPose& pose);
    [[nodiscard]] ListenerPose listener() const;

    // ---- Headless mixing ---------------------------------------------------
    // Mix the next `out.size() / channels()` frames into `out` (interleaved
    // float). Only for no_device engines — a device engine's mix belongs to
    // the audio thread.
    [[nodiscard]] std::expected<void, core::Error> read_frames(std::span<float> out);

private:
    AudioEngine() = default;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace engine::audio
