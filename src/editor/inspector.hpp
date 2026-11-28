#pragma once

// Inspector panel (Phase 9, Slice 9.4). Private editor header. The Renderer
// panel lives in tuning_panels.hpp.

#include <entt/entity/entity.hpp>

namespace engine::scene {
class Scene;
}

namespace engine::editor {

namespace panels {
struct InspectorState;
}

// Draws the "Inspector" window for `selected` (component editors via
// ComponentInspector<T> specialisations in the .cpp) when `*open` (null:
// always, no close button). `state` holds the Add Component search and the
// copied component values (EditorShell owns it).
void draw_inspector(scene::Scene& scene, entt::entity selected, panels::InspectorState& state,
                    bool* open = nullptr);

} // namespace engine::editor
