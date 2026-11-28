// Inspector section: Audio Listener (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../audio_inspector.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::AudioListener> {
    static constexpr const char* title = "Audio Listener";
    static constexpr const char* codec = "audio_listener";
    static constexpr const char* icon = icon::ear;
    static void draw(entt::registry& r, entt::entity e, scene::AudioListener& listener) {
        draw_audio_listener(r, e, listener);
    }
};

void register_audio_listener_inspector(InspectorRegistry& registry) {
    registry.add({.name = "audio_listener",
                  .order = 2600,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::AudioListener>(state, r, e, true);
        }});
}

} // namespace engine::editor
