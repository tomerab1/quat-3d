#pragma once

// Inspector panel (Phase 9, Slice 9.4). Private editor header. The Renderer
// panel lives in tuning_panels.hpp.

#include <entt/entity/entity.hpp>

namespace engine::scene {
class Scene;
}
namespace engine::script {
class ScriptRuntime;
}

namespace engine::editor {

// Draws the "Inspector" window for `selected` (component editors via
// ComponentInspector<T> specialisations in the .cpp). `scripts` (may be
// null) describes the properties a Script component's file declares.
void draw_inspector(scene::Scene& scene, entt::entity selected, script::ScriptRuntime* scripts);

} // namespace engine::editor
