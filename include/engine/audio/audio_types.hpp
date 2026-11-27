#pragma once

// Plain audio vocabulary shared by the AudioEngine, the AudioSource component
// and the JSON codec (ROADMAP G1). Header-only and dependency-free so
// components.hpp can name buses without pulling in the audio library.

#include <array>
#include <cstdint>
#include <string_view>

namespace engine::audio {

// Mixer buses. Every sound feeds exactly one bus; music / sfx / ui / ambience
// are sub-mixes of `master` (the engine output), each with its own volume.
enum class AudioBus : std::uint8_t { master, music, sfx, ui, ambience };
inline constexpr std::size_t bus_count = 5;

inline constexpr std::array<std::string_view, bus_count> bus_names = {"master", "music", "sfx",
                                                                       "ui", "ambience"};

[[nodiscard]] constexpr std::string_view bus_name(AudioBus bus) {
    const auto i = static_cast<std::size_t>(bus);
    return i < bus_count ? bus_names[i] : std::string_view("unknown");
}

// Long-form buses stream their clips from disk (bounded memory, decode on the
// resource manager's job thread); short sfx / ui clips are decoded fully into
// memory once and shared by every sound playing them (low-latency starts).
[[nodiscard]] constexpr bool bus_streams_by_default(AudioBus bus) {
    return bus == AudioBus::music || bus == AudioBus::ambience;
}

// Playback state of one sound. Paused keeps the play cursor; stopped rewinds.
enum class Playback : std::uint8_t { stopped, playing, paused };

// Opaque sound id: slot index in the low 16 bits, slot generation in the high
// 16, so a stale handle never aliases a sound created later in the same slot.
using SoundHandle = std::uint32_t;
inline constexpr SoundHandle invalid_sound = 0xFFFFFFFFU;

// Owner tag for sounds created on behalf of something (the ECS passes the
// entity id) — see AudioEngine::owned_sounds().
inline constexpr std::uint32_t no_owner = 0xFFFFFFFFU;

} // namespace engine::audio
