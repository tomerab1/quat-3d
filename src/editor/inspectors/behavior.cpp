// Inspector section: Behavior Tree (ROADMAP S9: one file per component).
// Registered by the generated inspectors.cpp; see inspectors/inspect.hpp.

#include <string>

#include "inspect.hpp"

#include <imgui_stdlib.h> // after imgui.h (inspect.hpp)

#include "../behavior_panel.hpp"
#include "../ui/fonts.hpp"
#include "engine/scene/behavior.hpp"

namespace engine::editor {

template <>
struct ComponentInspector<scene::BehaviorTree> {
    static constexpr const char* title = "Behavior Tree";
    static constexpr const char* codec = "behavior";
    static constexpr const char* icon = icon::workflow;
    static bool* enabled(scene::BehaviorTree& bt) { return &bt.enabled; }
    static void draw(entt::registry&, entt::entity, scene::BehaviorTree& bt) {
        if (ui::button(ui::label(icon::workflow, "Edit graph").c_str(), ui::ButtonKind::primary,
                       ImVec2(-1.0F, 0.0F))) {
            ImGui::SetWindowFocus(behavior_panel_title);
        }
        // Resizable: trees with saved editor layout outgrow any fixed buffer.
        {
            ui::FontScope mono(ui::FontRole::mono);
            ImGui::InputTextMultiline("##bt_json", &bt.source,
                                      ImVec2(-1.0F, ImGui::GetTextLineHeight() * 8.0F));
        }
        if (ui::button(ui::label(icon::refresh_cw, "Recompile").c_str(), ui::ButtonKind::secondary,
                       ImVec2(-1.0F, 0.0F))) {
            bt.compiled.reset();
            bt.parse_failed = false;
        }
        if (bt.parse_failed) {
            ui::text_colored(ui::tokens().error, ui::label(icon::circle_x, "Parse failed (see the Console)"));
        } else if (bt.compiled != nullptr) {
            ui::text_colored(ui::tokens().success,
                             ui::label(icon::circle_check, "Compiled: " +
                                                               std::to_string(bt.compiled->nodes().size()) +
                                                               " nodes"));
        }
        std::string leaves;
        for (const scene::BtLeafInfo& leaf : scene::bt_builtin_leaf_info()) {
            leaves += (leaves.empty() ? "" : ", ") + std::string(leaf.name);
        }
        const std::string text = "JSON: sequence/selector (resume the running child), "
                                 "reactive_sequence/reactive_selector (re-check every tick, "
                                 "pre-empting), inverter; leaves " + leaves + ".";
        hint(text.c_str());
    }
};

void register_behavior_inspector(InspectorRegistry& registry) {
    registry.add({.name = "behavior",
                  .order = 1900,
                  .draw = [](panels::InspectorState& state, entt::registry& r, entt::entity e) {
            inspect<scene::BehaviorTree>(state, r, e, true);
        }});
}

} // namespace engine::editor
