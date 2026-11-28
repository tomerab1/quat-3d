// Shell dialogs (ROADMAP E8.2): File > Open / Save As, Help > Keyboard
// Shortcuts (generated from the command registry, so it is always the real
// table), Help > About and Edit > Preferences.

#include <algorithm>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "../ui/appearance.hpp"
#include "../ui/fonts.hpp"
#include "../ui/icons.hpp"
#include "../ui/metrics.hpp"
#include "../ui/widgets.hpp"
#include "editor_shell.hpp"

namespace engine::editor::shell {

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return {s.begin(), s.end()};
}

// Scene files (*.json) under <root>/scenes and <root>/assets, project-relative.
std::vector<std::string> find_scene_files(const fs::path& root) {
    std::vector<std::string> out;
    for (const char* sub : {"scenes", "assets"}) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root / sub, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (out.size() >= 2000) break;
            if (it->is_regular_file(ec) && it->path().extension() == ".json") {
                out.push_back(utf8(it->path().lexically_relative(root)));
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void centre_next_window(float width) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(ui::scaled(width), 0.0F), ImGuiCond_Appearing);
}

void open_dialog(EditorShell& shell) {
    const char* id = "Open Scene###open_scene";
    if (shell.open_dialog) {
        ImGui::OpenPopup(id);
        shell.dialog = {};
        shell.dialog.path = shell.scene_path;
        shell.dialog.files = find_scene_files(shell.project_root);
        shell.open_dialog = false;
    }
    centre_next_window(520.0F);
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    (void)ui::search_field("filter", shell.dialog.filter, "Filter scene files");
    ImGui::BeginChild("##scenes", ImVec2(0.0F, ui::scaled(220.0F)), ImGuiChildFlags_Borders);
    int shown = 0;
    for (const std::string& f : shell.dialog.files) {
        if (!ui::matches_filter(f, shell.dialog.filter)) continue;
        ++shown;
        if (ImGui::Selectable(ui::label(icon::file_json, f).c_str(), f == shell.dialog.path,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            shell.dialog.path = f;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                shell.open_scene(f);
                ImGui::CloseCurrentPopup();
            }
        }
    }
    if (shown == 0) {
        (void)ui::empty_state(icon::file_json, "No scene files",
                              "Save a scene first (File > Save As), or type a path below.");
    }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##path", "scenes/my_scene.json", &shell.dialog.path);
    const char* problem = scene_path_problem(shell.dialog.path);
    if (problem != nullptr) ui::text_colored(ui::tokens().warning, problem);
    ImGui::Spacing();
    const float bw = ui::scaled(96.0F);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw * 2.0F - ui::scaled(ui::Spacing::md + ui::Spacing::xs));
    if (ui::button("Cancel", ui::ButtonKind::secondary, ImVec2(bw, 0.0F)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(problem != nullptr);
    if (ui::button("Open", ui::ButtonKind::primary, ImVec2(bw, 0.0F))) {
        shell.open_scene(shell.dialog.path);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void save_as_dialog(EditorShell& shell) {
    const char* id = "Save Scene As###save_scene_as";
    if (shell.save_as_dialog) {
        ImGui::OpenPopup(id);
        shell.dialog = {};
        shell.dialog.path = shell.scene_path;
        shell.save_as_dialog = false;
    }
    centre_next_window(460.0F);
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ui::text_secondary("Project-relative path of the scene file:");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0F);
    const bool enter = ImGui::InputTextWithHint("##path", "scenes/my_scene.json", &shell.dialog.path,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    const char* problem = scene_path_problem(shell.dialog.path);
    std::error_code ec;
    const bool exists =
        problem == nullptr && fs::exists(fs::path(shell.resolve_scene_path(shell.dialog.path)), ec);
    if (problem != nullptr) {
        ui::text_colored(ui::tokens().warning, problem);
    } else if (exists) {
        ui::text_colored(ui::tokens().warning, "The file exists and will be replaced");
    }
    ImGui::Spacing();
    const float bw = ui::scaled(96.0F);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw * 2.0F - ui::scaled(ui::Spacing::md + ui::Spacing::xs));
    if (ui::button("Cancel", ui::ButtonKind::secondary, ImVec2(bw, 0.0F)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(problem != nullptr);
    if (ui::button("Save", ui::ButtonKind::primary, ImVec2(bw, 0.0F)) || (enter && problem == nullptr)) {
        shell.save_scene_as(shell.dialog.path);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void shortcuts_window(EditorShell& shell) {
    if (!shell.shortcuts_open) return;
    centre_next_window(520.0F);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui::scaled(360.0F), ui::scaled(200.0F)),
                                        ImVec2(ui::scaled(900.0F), ui::scaled(640.0F)));
    if (ImGui::Begin(ui::label(icon::keyboard, "Keyboard Shortcuts###shortcuts").c_str(),
                     &shell.shortcuts_open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        // Grouped by category, in registration order.
        std::vector<std::string> order;
        std::map<std::string, std::vector<const Command*>> groups;
        for (const Command& c : shell.commands.all()) {
            if (c.shortcut == 0) continue;
            if (!groups.contains(c.category)) order.push_back(c.category);
            groups[c.category].push_back(&c);
        }
        ImGui::BeginChild("##list", ImVec2(0.0F, ui::scaled(420.0F)));
        for (const std::string& cat : order) {
            ImGui::SeparatorText(cat.c_str());
            if (ImGui::BeginTable(cat.c_str(), 2, ImGuiTableFlags_SizingStretchProp)) {
                for (const Command* c : groups[cat]) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(ui::label(c->icon, c->label).c_str());
                    ImGui::TableSetColumnIndex(1);
                    ui::FontScope mono(ui::FontRole::mono);
                    ui::text_colored(ui::tokens().accent_text, format_chord(c->shortcut));
                }
                ImGui::EndTable();
            }
        }
        ImGui::SeparatorText("Viewport");
        ui::text_secondary("Right-drag: look  -  WASD / arrows: fly  -  Q/E: down/up  -  Shift: fast");
        ui::text_secondary("Click: select  -  Ctrl+click: walk the selected agent there");
        ui::text_secondary("Shift+click: add a patrol waypoint  -  drag a glTF from Assets: place it");
        ImGui::EndChild();
    }
    ImGui::End();
}

void about_window(EditorShell& shell) {
    if (!shell.about_open) return;
    centre_next_window(420.0F);
    if (ImGui::Begin(ui::label(icon::info, "About###about").c_str(), &shell.about_open,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
        {
            ui::FontScope heading(ui::FontRole::heading);
            ui::text_colored(ui::tokens().accent_text, ui::label(icon::orbit, "quat-3d editor"));
        }
        ui::text_secondary("Vulkan 1.3 renderer, Jolt physics, Slang shaders.");
        ImGui::Separator();
        ui::text_secondary("UI: Dear ImGui " IMGUI_VERSION ", Inter and JetBrains Mono (SIL OFL),");
        ui::text_secondary("Lucide icons (ISC). Licences: assets/fonts/*/.");
    }
    ImGui::End();
}

void preferences_window(EditorShell& shell) {
    if (!shell.preferences_open) return;
    centre_next_window(440.0F);
    if (ImGui::Begin(ui::label(icon::settings, "Preferences###preferences").c_str(),
                     &shell.preferences_open,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        if (ui::begin_property_grid("prefs")) {
            static constexpr ui::EnumItem themes[] = {
                {static_cast<int>(ui::ThemeId::dark), "Dark", nullptr},
                {static_cast<int>(ui::ThemeId::light), "Light", nullptr},
            };
            ui::ThemeId theme = shell.appearance().theme_id();
            if (ui::prop_enum("Theme", theme, themes)) shell.choose_theme(theme);
            char scale[48];
            std::snprintf(scale, sizeof(scale), "%.0f %% (fonts at %.1fx density)",
                          static_cast<double>(shell.appearance().dpi().ui * 100.0F),
                          static_cast<double>(shell.appearance().dpi().raster));
            ui::prop_value("UI scale", scale, "Follows the display's scale (SDL3)");
            if (shell.ctx != nullptr && shell.ctx->camera_speed != nullptr) {
                (void)ui::prop_float("Camera speed", *shell.ctx->camera_speed,
                                     {.min = 0.1F, .max = 10.0F, .format = "%.1fx", .slider = true},
                                     {.reset = 1.0F});
            }
            ui::prop_separator("Snapping");
            (void)ui::prop_float("Move step", shell.snap.translate,
                                 {.speed = 0.01F, .min = SnapLimits::move_min, .max = SnapLimits::move_max, .format = "%.3f m", .clamp = true},
                                 {.reset = 0.5F});
            (void)ui::prop_float("Rotate step", shell.snap.rotate_deg,
                                 {.speed = 0.5F, .min = SnapLimits::rotate_min, .max = SnapLimits::rotate_max, .format = "%.1f°", .clamp = true},
                                 {.reset = 15.0F});
            (void)ui::prop_float("Scale step", shell.snap.scale,
                                 {.speed = 0.01F, .min = SnapLimits::scale_min, .max = SnapLimits::scale_max, .format = "%.3f", .clamp = true},
                                 {.reset = 0.1F});
            ui::end_property_grid();
        }
        ui::text_colored(ui::tokens().text_disabled,
                         "Saved with the layout in <project>/.quat/editor.ini");
    }
    ImGui::End();
}

} // namespace

void draw_dialogs(EditorShell& shell) {
    open_dialog(shell);
    save_as_dialog(shell);
    shortcuts_window(shell);
    about_window(shell);
    preferences_window(shell);
}

} // namespace engine::editor::shell
