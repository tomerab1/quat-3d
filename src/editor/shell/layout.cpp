// Default dock layout and persisted editor preferences (ROADMAP E8.2).
//
// Layout: Hierarchy left, Viewport centre, Inspector right (Renderer tabbed
// behind it), Assets / Console / Animation / Behaviour / Physics / Stats
// tabbed along the bottom of the left and centre columns.
// ImGui saves the dock tree to <project>/.quat/editor.ini; the
// "[QuatEditor][Prefs]" section next to it keeps the theme, closed panels,
// snapping and recent scenes.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder, ImGuiSettingsHandler

#include "editor_shell.hpp"

namespace engine::editor::shell {

namespace {

std::string g_ini_path; // ImGui keeps the pointer; lives for the process

void* settings_open(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name) {
    return std::strcmp(name, "Prefs") == 0 ? handler->UserData : nullptr;
}

void settings_read_line(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    auto* shell = static_cast<EditorShell*>(entry);
    char text[512];
    int value = 0;
    float f = 0.0F;
    if (std::sscanf(line, "Theme=%15s", text) == 1) {
        shell->appearance().set_theme(std::strcmp(text, "light") == 0 ? ui::ThemeId::light
                                                                      : ui::ThemeId::dark);
    } else if (std::sscanf(line, "Panel=%63[^,],%d", text, &value) == 2) {
        if (const auto p = find_panel(text); p && panel_info(*p).closable) {
            shell->panel_open[static_cast<std::size_t>(*p)] = value != 0;
        }
    } else if (std::sscanf(line, "SnapEnabled=%d", &value) == 1) {
        shell->snap.enabled = value != 0;
    } else if (std::sscanf(line, "SnapMove=%f", &f) == 1 && f > 0.0F) {
        shell->snap.translate = f;
    } else if (std::sscanf(line, "SnapRotate=%f", &f) == 1 && f > 0.0F) {
        shell->snap.rotate_deg = f;
    } else if (std::sscanf(line, "SnapScale=%f", &f) == 1 && f > 0.0F) {
        shell->snap.scale = f;
    } else if (std::sscanf(line, "ViewportStats=%d", &value) == 1) {
        shell->show_viewport_stats = value != 0;
    } else if (std::strncmp(line, "Recent=", 7) == 0 && line[7] != '\0') {
        shell->recent_scenes.emplace_back(line + 7);
    } else if (std::strncmp(line, "Scene=", 6) == 0 && line[6] != '\0') {
        shell->scene_path = line + 6;
    }
}

void settings_write_all(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
    auto* shell = static_cast<EditorShell*>(handler->UserData);
    buf->appendf("[%s][Prefs]\n", handler->TypeName);
    const bool light = shell->appearance().theme_id() == ui::ThemeId::light;
    buf->appendf("Theme=%s\n", light ? "light" : "dark");
    for (const PanelInfo& p : panel_infos()) {
        if (p.closable) {
            buf->appendf("Panel=%s,%d\n", p.title,
                         shell->panel_open[static_cast<std::size_t>(p.id)] ? 1 : 0);
        }
    }
    buf->appendf("SnapEnabled=%d\nSnapMove=%g\nSnapRotate=%g\nSnapScale=%g\n",
                 shell->snap.enabled ? 1 : 0, static_cast<double>(shell->snap.translate),
                 static_cast<double>(shell->snap.rotate_deg),
                 static_cast<double>(shell->snap.scale));
    buf->appendf("ViewportStats=%d\n", shell->show_viewport_stats ? 1 : 0);
    buf->appendf("Scene=%s\n", shell->scene_path.c_str());
    for (const std::string& r : shell->recent_scenes) buf->appendf("Recent=%s\n", r.c_str());
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
    g_ini_path.assign(p.begin(), p.end());
    ImGui::GetIO().IniFilename = g_ini_path.c_str();
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

    ImGui::DockBuilderDockWindow(panel_window_id(Panel::viewport), centre);
    ImGui::DockBuilderDockWindow(panel_window_id(Panel::hierarchy), left);
    for (const Panel p : {Panel::inspector, Panel::renderer}) {
        ImGui::DockBuilderDockWindow(panel_window_id(p), right);
    }
    for (const Panel p : {Panel::assets, Panel::console, Panel::animation, Panel::behaviour,
                          Panel::physics, Panel::stats}) {
        ImGui::DockBuilderDockWindow(panel_window_id(p), bottom);
    }
    // The viewport has no tab bar: it is the document, not a tool.
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(centre)) {
        node->LocalFlags |= ImGuiDockNodeFlags_HiddenTabBar;
    }
    // First tab of each group in front.
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(right)) {
        node->SelectedTabId = ImHashStr(panel_window_id(Panel::inspector));
    }
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(bottom)) {
        node->SelectedTabId = ImHashStr(panel_window_id(Panel::assets));
    }
    ImGui::DockBuilderFinish(dockspace_id);
}

} // namespace engine::editor::shell
