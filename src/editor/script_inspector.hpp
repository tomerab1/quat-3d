#pragma once

// Editor UI for gameplay scripts (G7). Private editor header: the Script
// component's inspector (inspector.cpp delegates here, keeping that file
// within the size rule) and the Stats panel's "Scripts" section.

#include <entt/entity/fwd.hpp>

namespace engine::scene {
struct Script;
}
namespace engine::script {
class ScriptRuntime;
}

namespace engine::editor {

// Path (typed or picked from assets/scripts/), enabled, the runtime status
// of the file, and the script's declared properties with their overrides.
// `scripts` may be null (no runtime: only path / enabled / overrides).
// Returns false when the user removed the component.
bool draw_script_component(entt::registry& registry, entt::entity entity, scene::Script& script,
                           script::ScriptRuntime* scripts);

// One row per script file the runtime knows: state, instances, reloads and
// the last error (failed scripts open by default, like shaders).
// The Script component's fields without a section header (the inspector
// draws the header).
void draw_script_fields(scene::Script& script, script::ScriptRuntime* scripts);
void draw_script_status(const script::ScriptRuntime* scripts);

} // namespace engine::editor
