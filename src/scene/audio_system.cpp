// ECS <-> audio bridge system — see include/engine/scene/audio_system.hpp.

#include "engine/scene/audio_system.hpp"

#include <cstdint>
#include <cstdio>

#include <entt/entity/registry.hpp>

#include "engine/audio/audio_engine.hpp"
#include "engine/scene/components.hpp"

namespace engine::scene {

namespace {

// Anything moving faster than sound between two frames teleported (spawn,
// respawn, snapshot restore): its doppler velocity is zero, not a shriek.
constexpr float max_doppler_speed = 343.0F;

// Identity of the sound a source needs: a change rebuilds it. Never 0, so a
// default-constructed component always resolves.
[[nodiscard]] std::uint64_t sound_key(const AudioSource& s) {
    std::uint64_t h = 14695981039346656037ULL; // FNV-1a
    const auto mix = [&h](std::uint8_t byte) {
        h ^= byte;
        h *= 1099511628211ULL;
    };
    for (const char c : s.clip) mix(static_cast<std::uint8_t>(c));
    mix(0);
    mix(static_cast<std::uint8_t>(s.bus));
    mix(s.spatial ? 1 : 0);
    return h | 1ULL;
}

[[nodiscard]] glm::vec3 velocity_of(const glm::vec3& from, const glm::vec3& to, float dt) {
    if (dt <= 0.0F) return glm::vec3(0.0F);
    const glm::vec3 v = (to - from) / dt;
    return glm::length(v) > max_doppler_speed ? glm::vec3(0.0F) : v;
}

[[nodiscard]] audio::ListenerPose pose_of(const glm::mat4& world) {
    audio::ListenerPose pose;
    pose.position = glm::vec3(world[3]);
    pose.forward = -glm::vec3(world[2]); // the engine looks down local -Z
    pose.up = glm::vec3(world[1]);
    return pose;
}

void update_listener(const entt::registry& registry, audio::AudioEngine& engine, float dt,
                     const AudioSystemParams& params) {
    std::optional<glm::mat4> world;
    for (const auto [e, listener, t] : registry.view<const AudioListener, const Transform>().each()) {
        if (listener.active) {
            world = t.world;
            break;
        }
    }
    if (!world) world = params.camera_world;
    if (!world) return;
    audio::ListenerPose pose = pose_of(*world);
    pose.velocity = velocity_of(engine.listener().position, pose.position, dt);
    engine.set_listener(pose);
}

} // namespace

void audio_system(entt::registry& registry, audio::AudioEngine& engine, float dt,
                  const AudioSystemParams& params) {
    // 0. Destroy sounds whose owner is gone: the entity was destroyed, lost
    // its AudioSource, or its component now refers to another sound (scene
    // restore, clip edit). Same ownership sweep as physics_system's bodies.
    for (const audio::AudioEngine::OwnedSound& os : engine.owned_sounds()) {
        const auto e = static_cast<entt::entity>(os.owner);
        const auto* src = registry.valid(e) ? registry.try_get<AudioSource>(e) : nullptr;
        if (src == nullptr || src->sound != os.sound) engine.destroy(os.sound);
    }

    update_listener(registry, engine, dt, params);

    for (auto [e, src] : registry.view<AudioSource>().each()) {
        const auto owner = static_cast<std::uint32_t>(entt::to_integral(e));
        // A component copied from another entity (duplicate, paste) carries
        // that entity's handle: this one needs its own sound.
        if (src.sound != audio::invalid_sound && engine.owner(src.sound) != owner) {
            src.sound = audio::invalid_sound;
            src.resolved = 0;
        }
        const std::uint64_t key = sound_key(src);
        if (src.sound != audio::invalid_sound && src.resolved != key) {
            engine.destroy(src.sound);
            src.sound = audio::invalid_sound;
            src.resolved = 0;
        }

        const glm::vec3 position = [&] {
            const auto* t = registry.try_get<Transform>(e);
            return t != nullptr ? glm::vec3(t->world[3]) : glm::vec3(0.0F);
        }();
        const bool autostart = params.simulate && src.play_on_start && !src.started;
        if (params.simulate) src.started = true;

        // Play mode loads every source up front; edit mode on the first
        // command. A failed load retries on the next explicit command, or
        // once the clip / bus / spatial flag change.
        bool fresh = false;
        const bool commanded = src.command != AudioCommand::none;
        if (src.sound == audio::invalid_sound && !src.clip.empty() &&
            (commanded || (params.simulate && src.resolved != key))) {
            audio::SoundDesc desc;
            desc.path = src.clip;
            desc.bus = src.bus;
            desc.stream = audio::bus_streams_by_default(src.bus);
            desc.spatial = src.spatial;
            desc.owner = owner;
            if (auto sound = engine.load(desc); sound) {
                src.sound = *sound;
                fresh = true;
            } else {
                std::fprintf(stderr, "[audio] %s\n", sound.error().message.c_str());
            }
            src.resolved = key;
        }
        if (src.sound == audio::invalid_sound) {
            src.command = AudioCommand::none;
            src.state = audio::Playback::stopped;
            continue;
        }

        engine.set_volume(src.sound, src.volume);
        engine.set_pitch(src.sound, src.pitch);
        engine.set_looping(src.sound, src.loop);
        if (src.spatial) {
            engine.set_spatial(src.sound, {src.min_distance, src.max_distance, src.rolloff,
                                           src.doppler});
            engine.set_position(src.sound, position);
            engine.set_velocity(src.sound, fresh ? glm::vec3(0.0F)
                                                 : velocity_of(src.last_position, position, dt));
        }
        src.last_position = position;

        if (autostart) engine.play(src.sound);
        switch (src.command) {
        case AudioCommand::play:
            engine.play(src.sound);
            break;
        case AudioCommand::stop:
            engine.stop(src.sound);
            break;
        case AudioCommand::pause:
            engine.pause(src.sound);
            break;
        case AudioCommand::resume:
            engine.resume(src.sound);
            break;
        case AudioCommand::none:
            break;
        }
        src.command = AudioCommand::none;
        src.state = engine.state(src.sound);
    }
}

} // namespace engine::scene
