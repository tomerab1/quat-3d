// Inspector section: Particle Emitter (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../particle_inspector.hpp"

namespace engine::editor {

// GPU particles (R10.3): the emitter's own editor (plain ImGui widgets, no
// property grid).
template <>
struct ComponentInspector<scene::ParticleEmitter> {
    static constexpr const char* title = "Particle Emitter";
    static constexpr const char* codec = "particle_emitter";
    static constexpr const char* icon = icon::sparkles;
    static void draw(entt::registry&, entt::entity, scene::ParticleEmitter& emitter) {
        draw_particle_emitter(emitter);
    }
};

void register_particle_emitter_inspector(InspectorRegistry& registry) {
    registry.add({.name = "particle_emitter",
                  .order = 1400,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::ParticleEmitter>(state, r, e, true);
        }});
}

} // namespace engine::editor
