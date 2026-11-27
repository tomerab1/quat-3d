#pragma once

// Audio wiring for the `game` executable (app layer, not engine): owns the
// AudioEngine and runs audio_system once per frame.

#include <optional>
#include <string>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include "engine/audio/audio_engine.hpp"

namespace quat::app {

class AudioRuntime {
public:
    // Opens the default playback device. Without one (headless machine, CI)
    // or with QUAT_NO_AUDIO=1 it falls back to a silent no-device engine, so
    // the audio components keep working; if even that fails, audio is off.
    // Relative clip paths resolve against `asset_root` (the project root).
    [[nodiscard]] static AudioRuntime create(const std::string& asset_root);

    // Once per frame, after the scene tick. `live` is play mode (always true
    // in builds without the editor): play_on_start sources start. Outside
    // play mode only the inspector's preview commands play. Every live <->
    // edit transition silences whatever was playing. `view` is the rendered
    // camera's view matrix — the listener when no AudioListener is active.
    void update(entt::registry& registry, const glm::mat4& view, bool live, float dt);

    [[nodiscard]] engine::audio::AudioEngine* engine() { return engine_ ? &*engine_ : nullptr; }

private:
    std::optional<engine::audio::AudioEngine> engine_;
    bool                                      was_live_ = false;
};

} // namespace quat::app
