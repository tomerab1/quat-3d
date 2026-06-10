#pragma once

// Inspector + Renderer panels (Phase 9, Slice 9.4). Private editor header.

#include <entt/entity/entity.hpp>

namespace engine::scene {
class Scene;
}

namespace engine::editor {

struct RendererSettings;

// Draws the "Inspector" window for `selected` (component editors via
// ComponentInspector<T> specialisations in the .cpp).
void draw_inspector(scene::Scene& scene, entt::entity selected);

// Draws the "Renderer" window: bloom parameters + IBL rebake request. The sun
// itself is edited through its DirectionalLight component in the Inspector.
void draw_renderer_panel(RendererSettings& settings);

} // namespace engine::editor
