// Inspector section: Script (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include "inspect.hpp"
#include "../script_inspector.hpp"

namespace engine::editor {

// Gameplay script (G7): file, enable, status and the file's declared
// properties (read from the script runtime).
template <>
struct ComponentInspector<scene::Script> {
    static constexpr const char* title = "Script";
    static constexpr const char* codec = "script";
    static constexpr const char* icon = icon::file_code;
    static void draw(entt::registry&, entt::entity, scene::Script& script,
                     const panels::InspectorState& state) {
        draw_script_fields(script, state.scripts);
    }
};

void register_script_inspector(InspectorRegistry& registry) {
    registry.add({.name = "script",
                  .order = 2100,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::Script>(state, r, e, true);
        }});
}

} // namespace engine::editor
