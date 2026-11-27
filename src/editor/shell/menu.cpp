// Main menu bar (ROADMAP E8.2): File / Edit / View / Create / Play / Help,
// built from registered commands so labels, icons, shortcuts and enabled
// states match the toolbar, palette and keyboard exactly.

#include <string>
#include <utility>

#include <imgui.h>

#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/widgets.hpp"
#include "editor_shell.hpp"
#include "scene_actions.hpp"

namespace engine::editor::shell {

namespace {

void item(EditorShell& shell, const char* id) {
    const Command* c = shell.commands.find(id);
    if (c == nullptr) return;
    const std::string text = ui::label(c->icon, c->label);
    const std::string keys = format_chord(c->shortcut);
    const bool checked = shell.commands.is_checked(*c);
    if (ImGui::MenuItem(text.c_str(), keys.empty() ? nullptr : keys.c_str(), checked,
                        shell.commands.is_enabled(*c))) {
        (void)shell.commands.run(id);
    }
    if (!c->hint.empty()) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", c->hint.c_str());
        }
    }
}

void file_menu(EditorShell& shell) {
    item(shell, "file.new");
    item(shell, "file.open");
    if (ImGui::BeginMenu(ui::label(icon::clock_3, "Open Recent").c_str(),
                         !shell.recent_scenes.empty())) {
        // Opening reorders recent_scenes: pick inside the loop, open after.
        std::string chosen;
        for (const std::string& path : shell.recent_scenes) {
            if (ImGui::MenuItem(path.c_str())) chosen = path;
        }
        if (!chosen.empty()) shell.open_scene(std::move(chosen));
        ImGui::Separator();
        if (ImGui::MenuItem("Clear Recent")) shell.recent_scenes.clear();
        ImGui::EndMenu();
    }
    ImGui::Separator();
    item(shell, "file.save");
    item(shell, "file.save_as");
    ImGui::Separator();
    item(shell, "file.exit");
}

void edit_menu(EditorShell& shell) {
    item(shell, "edit.undo");
    item(shell, "edit.redo");
    ImGui::Separator();
    item(shell, "edit.rename");
    item(shell, "edit.duplicate");
    item(shell, "edit.delete");
    item(shell, "edit.deselect");
    ImGui::Separator();
    item(shell, "edit.preferences");
}

void view_menu(EditorShell& shell) {
    ImGui::SeparatorText("Panels");
    for (const PanelInfo& p : panel_infos()) {
        item(shell, ((p.closable ? "view.panel." : "view.focus.") + std::string(p.title)).c_str());
    }
    ImGui::SeparatorText("Layout");
    item(shell, "view.reset_layout");
    item(shell, "view.viewport_stats");
    ImGui::SeparatorText("Theme");
    item(shell, "view.theme.dark");
    item(shell, "view.theme.light");
    ImGui::Separator();
    item(shell, "view.palette");
}

void create_menu(EditorShell& shell) {
    std::string_view group = "\x01";
    for (const CreateKind& k : create_kinds()) {
        if (k.group != group) {
            if (!k.group.empty()) ImGui::SeparatorText(std::string(k.group).c_str());
            group = k.group;
        }
        item(shell, ("create." + std::string(k.id)).c_str());
    }
}

void play_menu(EditorShell& shell) {
    item(shell, "play.play");
    item(shell, "play.pause");
    item(shell, "play.step");
    item(shell, "play.stop");
    ImGui::Separator();
    item(shell, "gizmo.translate");
    item(shell, "gizmo.rotate");
    item(shell, "gizmo.scale");
    item(shell, "gizmo.space");
    item(shell, "gizmo.snap");
}

void help_menu(EditorShell& shell) {
    item(shell, "help.shortcuts");
    item(shell, "view.palette");
    ImGui::Separator();
    item(shell, "help.about");
}

} // namespace

void draw_main_menu(EditorShell& shell) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ui::scaled(ui::Spacing::xs), ui::scaled(6.0F)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ui::scaled(ui::Spacing::xs), ui::scaled(ui::Spacing::xs)));
    if (!ImGui::BeginMainMenuBar()) {
        ImGui::PopStyleVar(2);
        return;
    }
    ImGui::PopStyleVar(); // menus keep the regular item spacing
    {
        ui::FontScope heading(ui::FontRole::heading);
        ui::text_colored(ui::tokens().accent_text, ui::label(icon::orbit, "quat-3d"));
    }
    ImGui::Dummy(ImVec2(ui::scaled(ui::Spacing::xxs), 0.0F));
    struct Menu {
        const char* title;
        void (*draw)(EditorShell&);
    };
    const Menu menus[] = {{"File", file_menu},     {"Edit", edit_menu}, {"View", view_menu},
                          {"Create", create_menu}, {"Play", play_menu}, {"Help", help_menu}};
    for (const Menu& m : menus) {
        if (ImGui::BeginMenu(m.title)) {
            m.draw(shell);
            ImGui::EndMenu();
        }
    }
    // Scene file on the right (the current document).
    const std::string scene = ui::label(icon::file_json, shell.scene_path);
    const float w = ImGui::CalcTextSize(scene.c_str()).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - ui::scaled(ui::Spacing::md));
    ui::text_secondary(scene);
    const std::string save_keys = shell.shortcut_text("file.save");
    ui::item_tooltip(save_keys.empty() ? "Current scene file"
                                       : ("Current scene file (" + save_keys + " saves it)").c_str());
    ImGui::EndMainMenuBar();
    ImGui::PopStyleVar();
}

} // namespace engine::editor::shell
