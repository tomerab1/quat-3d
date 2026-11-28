// Inspector section: Material (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../tuning_panels.hpp"

namespace engine::editor {

// The per-entity material overrides (A8) of whatever the entity draws: no
// component of its own, so no codec-backed section header.
void register_material_inspector(InspectorRegistry& registry) {
    registry.add({.name = "material",
                  .order = 400,
                  .draw = [](panels::InspectorState& /*state*/, entt::registry& r, entt::entity e) {
            draw_material_inspector(r, e);
        }});
}

} // namespace engine::editor
