// Inspector section: Audio Source (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../audio_inspector.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::AudioSource> {
    static constexpr const char* title = "Audio Source";
    static constexpr const char* codec = "audio_source";
    static constexpr const char* icon = icon::volume_2;
    static void draw(entt::registry&, entt::entity, scene::AudioSource& source) {
        draw_audio_source(source);
    }
};

void register_audio_source_inspector(InspectorRegistry& registry) {
    registry.add({.name = "audio_source",
                  .order = 2500,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::AudioSource>(state, r, e, true);
        }});
}

} // namespace engine::editor
