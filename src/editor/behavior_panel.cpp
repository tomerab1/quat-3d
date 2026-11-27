#include "behavior_panel.hpp"

#include <algorithm>
#include <utility>

#include <entt/entity/registry.hpp>
#include <imgui.h>
// vcpkg installs the header under include/imgui-node-editor/; a plain source
// checkout (local compile checks) has it at the repository root.
#if __has_include(<imgui-node-editor/imgui_node_editor.h>)
#include <imgui-node-editor/imgui_node_editor.h>
#else
#include <imgui_node_editor.h>
#endif

#include "engine/scene/behavior.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "behavior_panel_style.hpp"
#include "shell/panels.hpp"
#include "ui/icons.hpp"
#include "ui/widgets.hpp"

namespace engine::editor {

namespace ned = ax::NodeEditor;

BehaviorPanel::BehaviorPanel() {
    ned::Config config;
    config.SettingsFile = nullptr; // layout lives in the tree JSON ("editor")
    ctx_ = ned::CreateEditor(&config);
    ned::SetCurrentEditor(ctx_);
    ned::Style& style = ned::GetStyle();
    style.SourceDirection = ImVec2(0.0F, 1.0F); // links leave parents downward
    style.TargetDirection = ImVec2(0.0F, -1.0F); // ...and enter children from above
    style.NodeRounding = 6.0F;
    style.NodePadding = ImVec4(8.0F, 2.0F, 8.0F, 2.0F);
    style.LinkStrength = 70.0F;
    style.PinRounding = 3.0F;
    style.Colors[ned::StyleColor_Bg] = ImVec4(0.09F, 0.10F, 0.11F, 1.0F);
    style.Colors[ned::StyleColor_Grid] = ImVec4(1.0F, 1.0F, 1.0F, 0.05F);
    style.Colors[ned::StyleColor_NodeBg] = ImVec4(0.15F, 0.16F, 0.17F, 0.96F);
    ned::SetCurrentEditor(nullptr);
}

BehaviorPanel::~BehaviorPanel() {
    if (ctx_ != nullptr) ned::DestroyEditor(ctx_);
}

void BehaviorPanel::note(Change change) {
    pending_ = std::max(pending_, change);
}

void BehaviorPanel::load(entt::entity entity, const scene::BehaviorTree& bt) {
    entity_ = entity;
    loaded_source_ = bt.source;
    load_error_.clear();
    compile_error_.clear();
    graph_ = {};
    if (!bt.source.empty()) {
        if (auto g = scene::bt_graph_from_json(bt.source)) {
            graph_ = std::move(*g);
        } else {
            load_error_ = g.error().message;
        }
    }
    issues_ = graph_.nodes.empty() ? std::vector<BtGraphIssue>{}
                                   : scene::bt_graph_validate(graph_);
    unapplied_ = false;
    pending_ = Change::none;
    dragging_layout_ = false;
    push_all_positions_ = true;
    push_positions_.clear();
    navigate_in_frames_ = 2; // node sizes are known after their first frame
    focus_node_ = 0;
    context_node_ = 0;
    context_link_child_ = 0;
    create_from_pin_ = 0;
}

void BehaviorPanel::draw(scene::Scene& scene, entt::entity selected) {
    const bool visible = ImGui::Begin(shell::panel_window_name(shell::Panel::behaviour), nullptr,
                                      ImGuiWindowFlags_NoScrollbar |
                                          ImGuiWindowFlags_NoScrollWithMouse);
    if (!visible) {
        ImGui::End();
        return;
    }
    entt::registry& r = scene.registry();
    const bool valid = selected != entt::null && r.valid(selected);
    auto* bt = valid ? r.try_get<scene::BehaviorTree>(selected) : nullptr;
    if (bt == nullptr) {
        entity_ = entt::null;
        if (ui::empty_state(icon::workflow,
                            valid ? "No behaviour tree" : "Nothing selected",
                            valid ? "Give this entity a Behavior Tree to author its AI as a graph."
                                  : "Select an entity to edit its behaviour tree.",
                            valid ? "Add Behavior Tree" : nullptr)) {
            r.emplace<scene::BehaviorTree>(selected);
        }
        ImGui::End();
        return;
    }
    if (selected != entity_ || bt->source != loaded_source_) load(selected, *bt);

    draw_toolbar(*bt);
    if (!load_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("Cannot show this tree as a graph: %s", load_error_.c_str());
        ImGui::PopStyleColor();
        ImGui::TextWrapped("Fix the JSON in the Inspector, or start over.");
        if (ImGui::Button("Replace with an empty tree")) {
            bt->source.clear();
            bt->compiled.reset();
            bt->parse_failed = false;
        }
        ImGui::End();
        return;
    }

    const std::size_t lines = issues_.size() + (compile_error_.empty() ? 0U : 1U);
    const float issues_height =
        lines == 0 ? 0.0F
                   : ImGui::GetTextLineHeightWithSpacing() *
                             static_cast<float>(std::min<std::size_t>(lines, 5)) +
                         ImGui::GetStyle().WindowPadding.y * 2.0F;
    const float canvas_height = std::max(ImGui::GetContentRegionAvail().y - issues_height, 60.0F);

    ned::SetCurrentEditor(ctx_);
    draw_canvas(*bt, canvas_height);
    ned::SetCurrentEditor(nullptr);
    if (lines > 0) draw_issues();
    if (pending_ != Change::none) commit(*bt);
    ImGui::End();
}

void BehaviorPanel::draw_toolbar(scene::BehaviorTree& bt) {
    ImGui::Checkbox("enabled", &bt.enabled);
    ImGui::SameLine();
    if (ImGui::Button("Auto layout") && !graph_.nodes.empty()) {
        scene::bt_graph_auto_layout(graph_);
        push_all_positions_ = true;
        navigate_in_frames_ = 1;
        note(Change::layout);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit")) navigate_in_frames_ = 1;
    ImGui::SameLine();
    if (unapplied_) {
        ImGui::TextColored(kErrorColor, "%zu issue(s): edits not applied yet", issues_.size());
    } else if (bt.parse_failed) {
        ImGui::TextColored(kErrorColor, "runtime parse failed (see log)");
    } else if (bt.compiled != nullptr) {
        ImGui::TextDisabled("%zu nodes, compiled", graph_.nodes.size());
    } else {
        ImGui::TextDisabled("%zu nodes", graph_.nodes.size());
    }
}

void BehaviorPanel::draw_issues() {
    ImGui::BeginChild("bt_issues", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders);
    if (!compile_error_.empty()) {
        ImGui::TextColored(kErrorColor, "runtime: %s", compile_error_.c_str());
    }
    for (std::size_t i = 0; i < issues_.size(); ++i) {
        const BtGraphIssue& issue = issues_[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        if (ImGui::Selectable(issue.message.c_str()) && issue.node != 0) focus_node_ = issue.node;
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void BehaviorPanel::commit(scene::BehaviorTree& bt) {
    Change change = pending_;
    pending_ = Change::none;
    // Children always run in their on-screen left-to-right order.
    if (scene::bt_graph_sort_children_by_x(graph_)) change = Change::structure;
    issues_ = scene::bt_graph_validate(graph_);
    compile_error_.clear();
    if (!issues_.empty()) {
        unapplied_ = true;
        return;
    }
    auto text = scene::bt_graph_to_json(graph_);
    if (!text) {
        compile_error_ = text.error().message;
        unapplied_ = true;
        return;
    }
    if (change == Change::structure || unapplied_) {
        // The runtime is the authority; it should agree with validation.
        if (auto compiled = scene::BehaviorTreeAsset::parse(*text); !compiled) {
            compile_error_ = compiled.error().message;
            unapplied_ = true;
            return;
        }
        bt.source = std::move(*text);
        bt.compiled.reset(); // recompiled by bt_system, as the inspector's Recompile
        bt.parse_failed = false;
    } else {
        bt.source = std::move(*text); // layout only: the compiled tree still matches
    }
    loaded_source_ = bt.source;
    unapplied_ = false;
}

} // namespace engine::editor
