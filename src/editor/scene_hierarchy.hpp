#pragma once

// Hierarchy panel (Phase 9, Slice 9.3) — tree view of the scene's entities.
// Private editor header: included by editor sources only.

#include <entt/entity/entity.hpp>

namespace engine::scene {
class Scene;
}

namespace engine::editor {

// Draws the "Hierarchy" window: entity tree from Name/Parent/Children, click
// to select (updates `selected`), right-click context menus for create /
// duplicate / delete. Deleting the selection resets it to entt::null.
void draw_scene_hierarchy(scene::Scene& scene, entt::entity& selected);

} // namespace engine::editor
