// Default dock layout and persisted editor preferences (ROADMAP E8.2).
//
// Layout: Hierarchy left, Viewport centre, Inspector right (Renderer tabbed
// behind it), Assets / Console / Animation / Behaviour / Physics / Stats
// tabbed along the bottom of the left and centre columns.
// ImGui saves the dock tree to <project>/.quat/editor.ini; the
// "[QuatEditor][Prefs]" section next to it keeps the theme, closed panels,
// snapping and recent scenes.

#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder, ImGuiSettingsHandler

#include "editor_shell.hpp"
#include "prefs.hpp"

namespace engine::editor::shell {

namespace {

void* settings_open(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name) {
    return std::strcmp(name, "Prefs") == 0 ? handler->UserData : nullptr;
}

// The ini file is external data: every line goes through the validating
// parser (prefs.cpp) before it reaches the shell.
void settings_read_line(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    auto* shell = static_cast<EditorShell*>(entry);
    EditorPrefs prefs = shell->prefs();
    if (parse_pref_line(line, prefs)) shell->apply_prefs(prefs);
}

void settings_write_all(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
    const auto* shell = static_cast<const EditorShell*>(handler->UserData);
    buf->appendf("[%s][Prefs]\n", handler->TypeName);
    const std::string body = write_prefs(shell->prefs());
    buf->append(body.c_str(), body.c_str() + body.size());
    buf->append("\n");
}

} // namespace

void install_settings(EditorShell& shell, bool use_saved_layout) {
    ImGuiSettingsHandler handler;
    handler.TypeName = "QuatEditor";
    handler.TypeHash = ImHashStr("QuatEditor");
    handler.ReadOpenFn = settings_open;
    handler.ReadLineFn = settings_read_line;
    handler.WriteAllFn = settings_write_all;
    handler.UserData = &shell;
    ImGui::AddSettingsHandler(&handler);

    if (!use_saved_layout) return; // default layout, nothing read or written
    const std::filesystem::path dir = shell.project_root / ".quat";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "[editor] cannot create %s (%s); layout will not persist\n",
                     dir.string().c_str(), ec.message().c_str());
        return;
    }
    const std::u8string p = (dir / "editor.ini").u8string();
    shell.ini_path.assign(p.begin(), p.end());
    ImGui::GetIO().IniFilename = shell.ini_path.c_str();
}

void build_default_layout(unsigned int dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID rest = dockspace_id;
    const ImGuiID right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.24F, nullptr, &rest);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Down, 0.30F, nullptr, &rest);
    ImGuiID centre = 0;
    const ImGuiID left = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.22F, nullptr, &centre);

    ImGui::DockBuilderDockWindow(panel_window_id(Panel::viewport).c_str(), centre);
    ImGui::DockBuilderDockWindow(panel_window_id(Panel::hierarchy).c_str(), left);
    for (const Panel p : {Panel::inspector, Panel::renderer}) {
        ImGui::DockBuilderDockWindow(panel_window_id(p).c_str(), right);
    }
    for (const Panel p : {Panel::assets, Panel::console, Panel::animation, Panel::behaviour,
                          Panel::physics, Panel::stats}) {
        ImGui::DockBuilderDockWindow(panel_window_id(p).c_str(), bottom);
    }
    // The viewport has no tab bar: it is the document, not a tool.
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(centre)) {
        node->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar;
    }
    // First tab of each group in front.
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(right)) {
        node->SelectedTabId = ImHashStr(panel_window_id(Panel::inspector).c_str());
    }
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(bottom)) {
        node->SelectedTabId = ImHashStr(panel_window_id(Panel::assets).c_str());
    }
    ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace engine::editor::shell
