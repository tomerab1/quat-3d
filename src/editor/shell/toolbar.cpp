// Main toolbar (ROADMAP E8.2) under the menu bar: play controls, transform
// tool + space, snapping, camera speed, and a search box that opens the
// command palette. Every button runs a registered command.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>
#include <imgui_internal.h> // BeginViewportSideBar

#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/widgets.hpp"
#include "editor_shell.hpp"

namespace engine::editor::shell {

namespace {

// Toolbar button bound to a command: icon, tooltip "Label  (Shortcut)",
// checked state as the accent fill, disabled when the command is.
void command_button(EditorShell& shell, const char* id) {
    const Command* c = shell.commands.find(id);
    if (c == nullptr) return;
    const std::string keys = format_chord(c->shortcut);
    const bool enabled = shell.commands.is_enabled(*c);
    const std::string tip = c->hint.empty() ? c->label : c->label + "\n" + c->hint;
    if (ui::toggle_button(id, c->icon, shell.commands.is_checked(*c), tip.c_str(),
                          keys.empty() ? nullptr : keys.c_str(), enabled)) {
        (void)shell.commands.run(id);
    }
    ImGui::SameLine();
}

// Play controls: Play is the primary (accent) action while editing; while
// the simulation runs, Stop turns red and Play shows as active.
void play_controls(EditorShell& shell) {
    const ui::Theme& t = ui::tokens();
    const SimState state = shell.sim_state();
    const Command* play = shell.commands.find("play.play");
    const bool can_play = play != nullptr && shell.commands.is_enabled(*play);
    {
        const bool running = state == SimState::playing;
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ui::to_imvec4(running || can_play ? t.success : t.text_disabled));
        const std::string keys = shell.shortcut_text("play.play");
        if (ui::toggle_button("play.play", icon::play, running,
                              state == SimState::paused ? "Resume" : "Play",
                              keys.empty() ? nullptr : keys.c_str(), can_play)) {
            (void)shell.commands.run("play.play");
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    command_button(shell, "play.pause");
    command_button(shell, "play.step");
    {
        const bool live = state != SimState::editing;
        if (live) ImGui::PushStyleColor(ImGuiCol_Text, ui::to_imvec4(t.error));
        command_button(shell, "play.stop");
        if (live) ImGui::PopStyleColor();
    }
}

void snap_controls(EditorShell& shell) {
    command_button(shell, "gizmo.snap");
    // Increment for the active tool; the popover edits all three.
    char preview[32];
    switch (shell.gizmo_op) {
    case GizmoOp::translate: std::snprintf(preview, sizeof(preview), "%.2g m", static_cast<double>(shell.snap.translate)); break;
    case GizmoOp::rotate: std::snprintf(preview, sizeof(preview), "%.0f°", static_cast<double>(shell.snap.rotate_deg)); break;
    case GizmoOp::scale: std::snprintf(preview, sizeof(preview), "%.2g", static_cast<double>(shell.snap.scale)); break;
    }
    ImGui::SetNextItemWidth(ui::scaled(72.0F));
    ImGui::BeginDisabled(!shell.snap.enabled);
    if (ImGui::BeginCombo("##snap", preview, ImGuiComboFlags_HeightLargest)) {
        ImGui::PushItemWidth(ui::scaled(120.0F));
        // Clamped (typed values too) to the ranges the ini parser accepts.
        constexpr ImGuiSliderFlags clamp = ImGuiSliderFlags_AlwaysClamp;
        ImGui::DragFloat("Move (m)", &shell.snap.translate, 0.01F, SnapLimits::move_min,
                         SnapLimits::move_max, "%.3f", clamp);
        ImGui::DragFloat("Rotate (°)", &shell.snap.rotate_deg, 0.5F, SnapLimits::rotate_min,
                         SnapLimits::rotate_max, "%.1f", clamp);
        ImGui::DragFloat("Scale", &shell.snap.scale, 0.01F, SnapLimits::scale_min,
                         SnapLimits::scale_max, "%.3f", clamp);
        ImGui::PopItemWidth();
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ui::item_tooltip("Snap increment for the active tool");
    ImGui::SameLine();
}

void camera_speed(EditorShell& shell) {
    if (shell.ctx == nullptr || shell.ctx->camera_speed == nullptr) return;
    ImGui::AlignTextToFramePadding();
    ui::text_colored(ui::tokens().text_secondary, ui::ic(icon::gauge));
    ui::item_tooltip("Camera speed (fly with right-drag + WASD)");
    ImGui::SameLine(0.0F, ui::scaled(ui::Spacing::xxs));
    ImGui::SetNextItemWidth(ui::scaled(64.0F));
    ImGui::DragFloat("##camspeed", shell.ctx->camera_speed, 0.02F, 0.1F, 10.0F, "%.1fx",
                     ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
    ui::item_tooltip("Camera speed multiplier (drag, or double-click to type)");
    ImGui::SameLine();
}

// A search-box look-alike that opens the palette (VS Code / UE5 pattern).
void palette_button(EditorShell& shell, float width) {
    const ui::Theme& t = ui::tokens();
    const ImVec2 size(width, ImGui::GetFrameHeight());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##palette", size)) shell.palette_open = true;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ui::to_u32(hovered ? t.bg3 : t.bg2),
                      ui::scaled(ui::Radius::md));
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), ui::to_u32(t.border),
                ui::scaled(ui::Radius::md));
    const float pad = ImGui::GetStyle().FramePadding.x;
    const float ty = p.y + (size.y - ImGui::GetTextLineHeight()) * 0.5F;
    const std::string text = ui::label(icon::search, "Search commands");
    dl->AddText(ImVec2(p.x + pad, ty), ui::to_u32(t.text_secondary), text.c_str());
    const std::string keys = shell.shortcut_text("view.palette");
    const float kw = ImGui::CalcTextSize(keys.c_str()).x;
    dl->AddText(ImVec2(p.x + size.x - pad - kw, ty), ui::to_u32(t.text_disabled), keys.c_str());
    ui::item_tooltip("Command palette: every menu action, panel and tool");
}

} // namespace

void draw_main_toolbar(EditorShell& shell) {
    const ui::Theme& t = ui::tokens();
    const float height = std::round(ui::scaled(ui::Sizes::toolbar_height));
    // The row is centred in the bar above its bottom hairline, on a whole
    // pixel (icon buttons are round(frame height) squares).
    const float hairline = std::max(1.0F, std::round(ui::scaled(1.0F)));
    const float row = std::round(ImGui::GetFrameHeight());
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::to_imvec4(t.bg0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(ui::scaled(ui::Spacing::xs),
                               std::floor((height - hairline - row) * 0.5F)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar("##main_toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, height,
                                    flags)) {
        // Hairline under the bar separates it from the dock area.
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(wp.x, wp.y + ws.y - hairline),
                                                  ImVec2(wp.x + ws.x, wp.y + ws.y),
                                                  ui::to_u32(t.border));
        ui::begin_toolbar();
        play_controls(shell);
        ui::toolbar_separator();
        command_button(shell, "gizmo.translate");
        command_button(shell, "gizmo.rotate");
        command_button(shell, "gizmo.scale");
        ui::toolbar_separator();
        {
            const bool local = shell.gizmo_space == GizmoSpace::local;
            const std::string text =
                ui::label(local ? icon::box : icon::globe, local ? "Local" : "World");
            if (ui::button(text.c_str(), ui::ButtonKind::ghost)) (void)shell.commands.run("gizmo.space");
            const std::string keys = shell.shortcut_text("gizmo.space");
            ui::item_tooltip("Transform space: world axes or the object's own",
                             keys.empty() ? nullptr : keys.c_str());
            ImGui::SameLine();
        }
        snap_controls(shell);
        ui::toolbar_separator();
        camera_speed(shell);
        ui::end_toolbar();

        const float search_w = ui::scaled(260.0F);
        const float right = ImGui::GetWindowWidth() - search_w - ui::scaled(ui::Spacing::xs);
        if (right > ImGui::GetCursorPosX()) {
            ImGui::SameLine(right);
            palette_button(shell, search_w);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

} // namespace engine::editor::shell
