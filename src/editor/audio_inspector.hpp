#pragma once

// Inspector editors for the audio components (G1). Private editor header;
// inspector.cpp's ComponentInspector specialisations delegate here so the
// inspector file stays within the size rule.

#include <entt/entity/fwd.hpp>

namespace engine::scene {
struct AudioSource;
struct AudioListener;
} // namespace engine::scene

namespace engine::editor {

// Clip / bus / mix / 3D falloff fields plus Play / Pause / Stop preview
// buttons (they set AudioSource::command; the audio system plays the sound
// in edit mode too, so a clip can be auditioned without entering Play).
void draw_audio_source(scene::AudioSource& source);

// "active" with radio semantics: activating one listener deactivates the rest.
void draw_audio_listener(entt::registry& registry, entt::entity entity,
                         scene::AudioListener& listener);

} // namespace engine::editor
