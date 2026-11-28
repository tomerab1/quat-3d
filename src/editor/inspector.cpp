#include "inspector.hpp"

#include <algorithm>
#include <utility>

#include <imgui.h>

#include "engine/scene/scene.hpp"
#include "inspectors/inspect.hpp"
#include "panels/inspector_parts.hpp"
#include "shell/panels.hpp"
#include "ui/icons.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

void InspectorRegistry::add(InspectorEntry entry) {
    const auto at = std::ranges::upper_bound(entries_, entry.order, {},
                                             [](const InspectorEntry& e) { return e.order; });
    entries_.insert(at, std::move(entry));
}

const InspectorRegistry& inspector_registry() {
    static const InspectorRegistry registry = [] {
        InspectorRegistry r;
        register_engine_inspectors(r);
        return r;
    }();
    return registry;
}

void draw_inspector(scene::Scene& scene, entt::entity selected, panels::InspectorState& state,
                    bool* open) {
    if (open != nullptr && !*open) return;
    if (!ImGui::Begin(shell::panel_window_name(shell::Panel::inspector).c_str(), open)) {
        ImGui::End();
        return;
    }
    entt::registry& r = scene.registry();
    if (selected == entt::null || !r.valid(selected)) {
        (void)ui::empty_state(icon::mouse_pointer_2, "Nothing selected",
                              "Select an entity in the Hierarchy or click it in the viewport.");
        ImGui::End();
        return;
    }

    panels::entity_header(scene, selected);
    // One section per component (src/editor/inspectors/), in their order.
    for (const InspectorEntry& entry : inspector_registry().entries()) {
        entry.draw(state, r, selected);
    }

    panels::add_component_button(r, selected, state);
    ImGui::End();
}

} // namespace engine::editor
