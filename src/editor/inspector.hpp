#pragma once

// Inspector + Renderer panels (Phase 9, Slice 9.4). Private editor header.

#include <entt/entity/entity.hpp>

namespace engine::scene {
class Scene;
}
namespace engine::script {
class ScriptRuntime;
}

namespace engine::editor {

struct RendererSettings;

// Draws the "Inspector" window for `selected` (component editors via
// ComponentInspector<T> specialisations in the .cpp). `scripts` (may be
// null) describes the properties a Script component's file declares.
void draw_inspector(scene::Scene& scene, entt::entity selected, script::ScriptRuntime* scripts);

// Draws the "Renderer" window: sun direction (azimuth/elevation, applied to
// the scene's DirectionalLight live; IBL rebakes automatically on slider
// release), bloom parameters, and a manual IBL rebake button.
void draw_renderer_panel(RendererSettings& settings, scene::Scene* scene);

} // namespace engine::editor
