#pragma once

// Inspector panel (Phase 9, Slice 9.4). Private editor header. The Renderer
// panel lives in tuning_panels.hpp.

#include <entt/entity/entity.hpp>

namespace engine::scene {
class Scene;
}

namespace engine::editor {

// Draws the "Inspector" window for `selected` (component editors via
// ComponentInspector<T> specialisations in the .cpp).
void draw_inspector(scene::Scene& scene, entt::entity selected);

} // namespace engine::editor
