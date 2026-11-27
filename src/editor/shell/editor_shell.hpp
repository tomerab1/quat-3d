#pragma once

// EditorShell — the editor's frame (ROADMAP E8.2): main menu, toolbar,
// status bar, dockspace + default layout, the panels, the command palette,
// dialogs and toasts. EditorLayer (the public facade in
// engine/editor/editor.hpp) owns one and forwards to it; everything the UI
// remembers between frames lives here (never in the ECS).
//
// Files: editor_shell.cpp (state, frame order, commands), menu.cpp,
// toolbar.cpp, status_bar.cpp, command_palette.cpp, dialogs.cpp, layout.cpp
// (dock layout + persisted preferences), viewport.cpp (scene image, overlay
// toolbar, gizmo, picking). Panels live in src/editor/panels/ and the
// existing panel sources.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <entt/entity/entity.hpp>
#include <imgui.h>

#include "../asset_browser.hpp"
#include "../scene_hierarchy.hpp"
#include "../ui/appearance.hpp"
#include "commands.hpp"
#include "engine/editor/editor.hpp"
#include "panels.hpp"

struct SDL_Window;

namespace engine::editor {

class BehaviorPanel;

namespace panels {
struct ConsoleState;
} // namespace panels

namespace shell {

enum class GizmoOp : std::uint8_t { translate, rotate, scale };
enum class GizmoSpace : std::uint8_t { world, local };

struct SnapSettings {
    bool  enabled = false;
    float translate = 0.5F;   // metres
    float rotate_deg = 15.0F; // degrees
    float scale = 0.1F;       // scale units
};

// Simulation state shown in the toolbar / status bar.
enum class SimState : std::uint8_t { editing, playing, paused };

class EditorShell {
public:
    EditorShell(SDL_Window* window, std::filesystem::path project_root);
    ~EditorShell();
    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;

    // Before ImGui::NewFrame: theme / DPI changes. True when the font atlas
    // was rebuilt.
    [[nodiscard]] bool before_new_frame();
    void draw(const EditorContext& ctx);

    // ---- State read by EditorLayer / the frame loop ------------------------
    entt::entity  selected = entt::null;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    bool          viewport_hovered = false;
    float         viewport_rect[4]{0.0F, 0.0F, 0.0F, 0.0F}; // screen min x/y, max x/y

    ui::Appearance& appearance() { return appearance_; }
    void focus_panel(std::string title) { pending_focus_ = std::move(title); }
    void notify(NoticeKind kind, std::string text);
    // Loads the saved layout (QUAT_EDITOR_LAYOUT=default ignores and does not
    // write it).
    void set_use_saved_layout(bool use) { use_saved_layout_ = use; }

    // ---- State shared with the shell's own files ----------------------------
    // (public so menu.cpp / toolbar.cpp / ... stay free functions over it)
    CommandRegistry          commands;
    const EditorContext*     ctx = nullptr; // valid during draw()
    std::filesystem::path    project_root;
    std::array<bool, k_panel_count> panel_open{};
    GizmoOp                  gizmo_op = GizmoOp::translate;
    GizmoSpace               gizmo_space = GizmoSpace::world;
    SnapSettings             snap;
    bool                     show_viewport_stats = true;
    bool                     palette_open = false;
    bool                     shortcuts_open = false;
    bool                     about_open = false;
    bool                     preferences_open = false;
    bool                     open_dialog = false;    // File > Open (modal)
    bool                     save_as_dialog = false; // File > Save As (modal)
    bool                     reset_layout = false;   // rebuild the default layout next frame
    std::string              scene_path = "scenes/editor.json"; // project-relative
    std::vector<std::string> recent_scenes;          // newest first
    bool                     rename_pending = false; // F2: the hierarchy starts renaming
    AnimationPreviewState    anim_state;
    PhysicsDebugState        physics_state;
    HierarchyState                        hierarchy;
    AssetBrowserState                     assets;
    std::unique_ptr<BehaviorPanel>        behavior_panel;
    std::unique_ptr<panels::ConsoleState> console;

    [[nodiscard]] SimState sim_state() const;
    [[nodiscard]] bool     has_scene() const { return ctx != nullptr && ctx->scene != nullptr; }
    [[nodiscard]] bool     has_selection() const;
    [[nodiscard]] std::string resolve_scene_path(const std::string& relative) const;
    void open_scene(const std::string& relative);
    void save_scene_as(const std::string& relative);
    void add_recent(const std::string& relative);
    void show_panel(Panel p); // opens (if closable) and focuses

private:
    void register_commands();
    void register_create_commands();
    void dispatch_shortcuts();
    void draw_dockspace();
    void draw_panels();

    ui::Appearance appearance_;
    std::string    pending_focus_;
    int            focus_frames_ = 0;
    bool           layout_checked_ = false;
    bool           settings_installed_ = false;
    int            layout_focus_frames_ = 0;
    bool           use_saved_layout_ = true;
    float          last_dt_ = 0.0F;
};

// ---- Shell parts (one file each) -------------------------------------------
void draw_main_menu(EditorShell& shell);
void draw_main_toolbar(EditorShell& shell);
void draw_status_bar(EditorShell& shell);
void draw_command_palette(EditorShell& shell);
void draw_dialogs(EditorShell& shell); // open / save as / shortcuts / about / preferences
void draw_viewport(EditorShell& shell);
void draw_stats_panel(EditorShell& shell);
// Docks every panel into the default arrangement under `dockspace_id`.
void build_default_layout(unsigned int dockspace_id);
// Registers the "QuatEditor" ini section (theme, panels, snapping, recent
// scenes) and points ImGui's ini file at <project>/.quat/editor.ini.
void install_settings(EditorShell& shell, bool use_saved_layout);

} // namespace shell
} // namespace engine::editor
