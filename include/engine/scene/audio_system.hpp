#pragma once

// ECS <-> audio bridge (ROADMAP G1): drives an audio::AudioEngine from the
// AudioSource / AudioListener components (components.hpp).

#include <optional>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

namespace engine::audio {
class AudioEngine;
}

namespace engine::scene {

struct AudioSystemParams {
    // Play mode (true): every source's sound loads up front and sources with
    // play_on_start start once. Edit mode (false): nothing plays unless asked
    // through AudioSource::command (the inspector's preview buttons), and
    // sounds load on that first command.
    bool simulate = true;
    // World matrix of the camera the frame renders from. The listener follows
    // it when no active AudioListener entity exists (else it stays put).
    std::optional<glm::mat4> camera_world;
};

// Once per frame, after the transform system (reads Transform.world):
//   - destroys sounds whose owner entity is gone or lost its AudioSource;
//   - places the listener (first active AudioListener, else the camera);
//   - (re)loads each source's sound when its clip / bus / spatial flag
//     changed — a failed load logs and is retried only after an edit or on
//     the next explicit command;
//   - applies volume / pitch / looping / 3D parameters and the world
//     position (velocity from the frame delta for doppler);
//   - runs play_on_start and pending commands, and reports `state`.
void audio_system(entt::registry& registry, audio::AudioEngine& engine, float dt,
                  const AudioSystemParams& params = {});

} // namespace engine::scene
