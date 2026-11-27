#include "editor_shell.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#include <SDL3/SDL_events.h>
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder (default layout)

#include "../animation_preview.hpp"
#include "../asset_browser.hpp"
#include "../behavior_panel.hpp"
#include "../inspector.hpp"
#include "../panels/console.hpp"
#include "../physics_debug.hpp"
#include "../scene_hierarchy.hpp"
#include "../scene_snapshot.hpp"
#include "../tuning_panels.hpp"
#include "../ui/icons.hpp"
#include "../ui/toasts.hpp"
#include "engine/core/sim_clock.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "scene_actions.hpp"

namespace engine::editor::shell {

namespace {

constexpr std::size_t k_max_recent_scenes = 8;

[[nodiscard]] ui::ToastKind toast_kind(NoticeKind k) {
    switch (k) {
    case NoticeKind::info: return ui::ToastKind::info;
    case NoticeKind::success: return ui::ToastKind::success;
    case NoticeKind::warning: return ui::ToastKind::warning;
    case NoticeKind::error: return ui::ToastKind::error;
    }
    return ui::ToastKind::info;
}

// Makes the docked window `id` the visible tab of its group without moving
// keyboard focus.
void select_docked_tab(const char* id) {
    ImGuiWindow* w = ImGui::FindWindowByName(id);
    if (w != nullptr && w->DockNode != nullptr && w->DockNode->TabBar != nullptr) {
        w->DockNode->TabBar->NextSelectedTabId = w->TabId;
        w->DockNode->TabBar->SelectedTabId = w->TabId;
        w->DockNode->SelectedTabId = w->TabId;
    }
}

} // namespace

EditorShell::EditorShell(SDL_Window* window, std::filesystem::path root)
    : project_root(std::move(root)) {
    panel_open.fill(true);
    set_panel_open_flags(&panel_open);
    appearance_.init(window, project_root, ui::ThemeId::dark);
    console = std::make_unique<panels::ConsoleState>();
    register_commands();
}

EditorShell::~EditorShell() { set_panel_open_flags(nullptr); }

bool EditorShell::before_new_frame() {
    if (!settings_installed_) {
        // Before the first NewFrame, which is when ImGui reads the ini file.
        install_settings(*this, use_saved_layout_);
        settings_installed_ = true;
    }
    return appearance_.update();
}

void EditorShell::notify(NoticeKind kind, std::string text) {
    ui::notify(toast_kind(kind), std::move(text));
}

SimState EditorShell::sim_state() const {
    if (ctx == nullptr || ctx->play_mode == nullptr || !*ctx->play_mode) return SimState::editing;
    if (ctx->sim_clock != nullptr && ctx->sim_clock->state().paused) return SimState::paused;
    return SimState::playing;
}

bool EditorShell::has_selection() const {
    return has_scene() && selected != entt::null && ctx->scene->registry().valid(selected);
}

std::string EditorShell::resolve_scene_path(const std::string& relative) const {
    std::filesystem::path p(std::u8string(relative.begin(), relative.end()));
    if (p.is_relative()) p = project_root / p;
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

void EditorShell::add_recent(const std::string& relative) {
    std::erase(recent_scenes, relative);
    recent_scenes.insert(recent_scenes.begin(), relative);
    if (recent_scenes.size() > k_max_recent_scenes) recent_scenes.resize(k_max_recent_scenes);
}

void EditorShell::open_scene(const std::string& relative) {
    if (ctx == nullptr || ctx->load_request == nullptr || relative.empty()) return;
    *ctx->load_request = resolve_scene_path(relative);
    scene_path = relative;
    add_recent(relative);
}

void EditorShell::save_scene_as(const std::string& relative) {
    if (ctx == nullptr || ctx->save_request == nullptr || relative.empty()) return;
    *ctx->save_request = resolve_scene_path(relative);
    scene_path = relative;
    add_recent(relative);
}

void EditorShell::show_panel(Panel p) {
    panel_open[static_cast<std::size_t>(p)] = true;
    pending_focus_ = panel_info(p).title;
    focus_frames_ = 0;
}

// ---- Commands ----------------------------------------------------------------

void EditorShell::register_commands() {
    CommandRegistry& c = commands;
    const auto selection_ok = [this] { return has_selection(); };
    const auto play_ok = [this] { return ctx != nullptr && ctx->play_mode != nullptr; };

    // File.
    c.add({.id = "file.new", .label = "New Scene", .category = "File", .icon = icon::file_plus,
           .shortcut = ImGuiMod_Ctrl | ImGuiKey_N, .hint = "Replace the scene with an empty one",
           .enabled = [this] { return ctx != nullptr && ctx->new_scene_request != nullptr &&
                                      sim_state() == SimState::editing; },
           .action = [this] {
               *ctx->new_scene_request = true;
               selected = entt::null;
               scene_path = "scenes/untitled.json";
           }});
    c.add({.id = "file.open", .label = "Open Scene...", .category = "File",
           .icon = icon::folder_open, .shortcut = ImGuiMod_Ctrl | ImGuiKey_O,
           .hint = "Load a scene file from the project",
           .enabled = [this] { return ctx != nullptr && ctx->load_request != nullptr &&
                                      sim_state() == SimState::editing; },
           .action = [this] { open_dialog = true; }});
    c.add({.id = "file.save", .label = "Save Scene", .category = "File", .icon = icon::save,
           .shortcut = ImGuiMod_Ctrl | ImGuiKey_S,
           .enabled = [this] { return ctx != nullptr && ctx->save_request != nullptr; },
           .action = [this] { save_scene_as(scene_path); }});
    c.add({.id = "file.save_as", .label = "Save Scene As...", .category = "File",
           .icon = icon::save, .shortcut = ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S,
           .enabled = [this] { return ctx != nullptr && ctx->save_request != nullptr; },
           .action = [this] { save_as_dialog = true; }});
    c.add({.id = "file.exit", .label = "Exit", .category = "File", .icon = icon::log_out,
           .shortcut = ImGuiMod_Ctrl | ImGuiKey_Q, .action = [] {
               SDL_Event quit{};
               quit.type = SDL_EVENT_QUIT;
               (void)SDL_PushEvent(&quit);
           }});

    // Edit. Undo / redo arrive with the command stack (ROADMAP E1).
    c.add({.id = "edit.undo", .label = "Undo", .category = "Edit", .icon = icon::undo_2,
           .shortcut = ImGuiMod_Ctrl | ImGuiKey_Z, .hint = "Arrives with the undo stack (E1)",
           .enabled = [] { return false; }, .action = [] {}});
    c.add({.id = "edit.redo", .label = "Redo", .category = "Edit", .icon = icon::redo_2,
           .shortcut = ImGuiMod_Ctrl | ImGuiKey_Y, .hint = "Arrives with the undo stack (E1)",
           .enabled = [] { return false; }, .action = [] {}});
    c.add({.id = "edit.rename", .label = "Rename", .category = "Edit", .icon = icon::type,
           .shortcut = ImGuiKey_F2, .scope = ShortcutScope::selection, .enabled = selection_ok,
           .action = [this] {
               rename_pending = true;
               show_panel(Panel::hierarchy);
           }});
    c.add({.id = "edit.duplicate", .label = "Duplicate", .category = "Edit", .icon = icon::copy,
           .shortcut = ImGuiMod_Ctrl | ImGuiKey_D, .scope = ShortcutScope::selection,
           .enabled = selection_ok, .action = [this] {
               const entt::entity copy = duplicate_entity(*ctx->scene, selected);
               if (copy != entt::null) selected = copy;
           }});
    c.add({.id = "edit.delete", .label = "Delete", .category = "Edit", .icon = icon::trash_2,
           .shortcut = ImGuiKey_Delete, .scope = ShortcutScope::selection,
           .enabled = selection_ok,
           .action = [this] { delete_entity_tree(*ctx->scene, selected, selected); }});
    c.add({.id = "edit.deselect", .label = "Deselect", .category = "Edit",
           .icon = icon::mouse_pointer_2, .enabled = selection_ok,
           .action = [this] { selected = entt::null; }});
    c.add({.id = "edit.preferences", .label = "Preferences...", .category = "Edit",
           .icon = icon::settings, .shortcut = ImGuiMod_Ctrl | ImGuiKey_Comma,
           .action = [this] { preferences_open = true; }});

    // View: panels, layout, theme, palette.
    for (const PanelInfo& p : panel_infos()) {
        const Panel id = p.id;
        if (p.closable) {
            c.add({.id = std::string("view.panel.") + p.title,
                   .label = std::string(p.title) + " Panel",
                   .category = "View",
                   .icon = p.icon,
                   .hint = "Show or hide the panel",
                   .checked = [this, id] { return panel_open[static_cast<std::size_t>(id)]; },
                   .action = [this, id] {
                       bool& open = panel_open[static_cast<std::size_t>(id)];
                       open = !open;
                       if (open) show_panel(id);
                   }});
        } else {
            c.add({.id = std::string("view.focus.") + p.title,
                   .label = std::string("Focus ") + p.title,
                   .category = "View",
                   .icon = p.icon,
                   .action = [this, id] { show_panel(id); }});
        }
    }
    c.add({.id = "view.reset_layout", .label = "Reset Layout", .category = "View",
           .icon = icon::layout_dashboard, .hint = "Restore the default panel arrangement",
           .action = [this] {
               reset_layout = true;
               panel_open.fill(true);
           }});
    c.add({.id = "view.theme.dark", .label = "Dark Theme", .category = "View", .icon = icon::moon,
           .checked = [this] { return appearance_.theme_id() == ui::ThemeId::dark; },
           .action = [this] { appearance_.set_theme(ui::ThemeId::dark); }});
    c.add({.id = "view.theme.light", .label = "Light Theme", .category = "View",
           .icon = icon::sun_medium,
           .checked = [this] { return appearance_.theme_id() == ui::ThemeId::light; },
           .action = [this] { appearance_.set_theme(ui::ThemeId::light); }});
    c.add({.id = "view.viewport_stats", .label = "Viewport Statistics", .category = "View",
           .icon = icon::activity, .checked = [this] { return show_viewport_stats; },
           .action = [this] { show_viewport_stats = !show_viewport_stats; }});
    c.add({.id = "view.palette", .label = "Command Palette...", .category = "View",
           .icon = icon::command, .shortcut = ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P,
           .action = [this] { palette_open = true; }});
    c.add({.id = "view.palette_quick", .label = "Go to Command...", .category = "View",
           .icon = icon::search, .shortcut = ImGuiMod_Ctrl | ImGuiKey_P,
           .action = [this] { palette_open = true; }});

    // Play.
    c.add({.id = "play.play", .label = "Play", .category = "Play", .icon = icon::play,
           .shortcut = ImGuiKey_F5, .hint = "Simulate the scene (physics, AI, scripts)",
           .enabled = [this, play_ok] { return play_ok() && sim_state() != SimState::playing; },
           .action = [this] {
               if (ctx->sim_clock != nullptr) ctx->sim_clock->resume();
               *ctx->play_mode = true;
           }});
    c.add({.id = "play.pause", .label = "Pause", .category = "Play", .icon = icon::pause,
           .shortcut = ImGuiKey_F6,
           .enabled = [this] { return ctx != nullptr && ctx->sim_clock != nullptr &&
                                      sim_state() == SimState::playing; },
           .action = [this] { ctx->sim_clock->pause(); }});
    c.add({.id = "play.step", .label = "Step One Frame", .category = "Play",
           .icon = icon::step_forward, .shortcut = ImGuiKey_F10,
           .hint = "Advance the paused simulation by one 1/60 s step",
           .enabled = [this] { return ctx != nullptr && ctx->sim_clock != nullptr &&
                                      sim_state() != SimState::editing; },
           .action = [this] { (void)ctx->sim_clock->step(1); }});
    c.add({.id = "play.stop", .label = "Stop", .category = "Play", .icon = icon::square,
           .shortcut = ImGuiMod_Shift | ImGuiKey_F5, .hint = "Stop and restore the edited scene",
           .enabled = [this] { return sim_state() != SimState::editing; },
           .action = [this] {
               if (ctx->sim_clock != nullptr) ctx->sim_clock->resume();
               *ctx->play_mode = false;
           }});

    // Gizmo / viewport.
    c.add({.id = "gizmo.translate", .label = "Move Tool", .category = "Tools", .icon = icon::move,
           .shortcut = ImGuiKey_W, .scope = ShortcutScope::viewport,
           .checked = [this] { return gizmo_op == GizmoOp::translate; },
           .action = [this] { gizmo_op = GizmoOp::translate; }});
    c.add({.id = "gizmo.rotate", .label = "Rotate Tool", .category = "Tools",
           .icon = icon::rotate_3d, .shortcut = ImGuiKey_E, .scope = ShortcutScope::viewport,
           .checked = [this] { return gizmo_op == GizmoOp::rotate; },
           .action = [this] { gizmo_op = GizmoOp::rotate; }});
    c.add({.id = "gizmo.scale", .label = "Scale Tool", .category = "Tools", .icon = icon::scale_3d,
           .shortcut = ImGuiKey_R, .scope = ShortcutScope::viewport,
           .checked = [this] { return gizmo_op == GizmoOp::scale; },
           .action = [this] { gizmo_op = GizmoOp::scale; }});
    c.add({.id = "gizmo.space", .label = "Toggle Local / World Space", .category = "Tools",
           .icon = icon::globe, .shortcut = ImGuiKey_X, .scope = ShortcutScope::viewport,
           .checked = [this] { return gizmo_space == GizmoSpace::local; },
           .action = [this] {
               gizmo_space = gizmo_space == GizmoSpace::world ? GizmoSpace::local
                                                               : GizmoSpace::world;
           }});
    c.add({.id = "gizmo.snap", .label = "Toggle Snapping", .category = "Tools",
           .icon = icon::magnet, .shortcut = ImGuiKey_Y, .scope = ShortcutScope::viewport,
           .checked = [this] { return snap.enabled; },
           .action = [this] { snap.enabled = !snap.enabled; }});

    register_create_commands();

    // Help.
    c.add({.id = "help.shortcuts", .label = "Keyboard Shortcuts", .category = "Help",
           .icon = icon::keyboard, .shortcut = ImGuiKey_F1,
           .action = [this] { shortcuts_open = true; }});
    c.add({.id = "help.about", .label = "About quat-3d", .category = "Help", .icon = icon::info,
           .action = [this] { about_open = true; }});
}

void EditorShell::register_create_commands() {
    for (const CreateKind& kind : create_kinds()) {
        const CreateKind* k = &kind;
        commands.add({.id = std::string("create.") + std::string(kind.id),
                      .label = std::string("Create ") + std::string(kind.label),
                      .category = "Create",
                      .icon = kind.icon,
                      .hint = "Add it in front of the camera and select it",
                      .enabled = [this] { return has_scene(); },
                      .action = [this, k] {
                          const entt::entity e = create_entity_of(
                              *ctx->scene, *k, entt::null, spawn_point(ctx->view));
                          if (e != entt::null) selected = e;
                      }});
    }
}

void EditorShell::dispatch_shortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    ShortcutContext sc;
    sc.typing = io.WantTextInput;
    sc.viewport = viewport_hovered;
    sc.camera_look = viewport_hovered && io.MouseDown[ImGuiMouseButton_Right];
    const ImGuiWindow* nav = GImGui->NavWindow;
    const auto focused = [&](Panel p) {
        return nav != nullptr && nav->RootWindow != nullptr &&
               nav->RootWindow->ID == ImHashStr(panel_window_id(p));
    };
    sc.selection_focus = viewport_hovered || focused(Panel::hierarchy) || focused(Panel::viewport);
    if (palette_open) return; // the palette owns the keyboard while open
    (void)commands.dispatch_shortcuts(sc);
}

// ---- Frame -------------------------------------------------------------------

void EditorShell::draw_dockspace() {
    // Panels close from the View menu / their tab, not a per-node button.
    const ImGuiID dockspace_id = ImGui::DockSpaceOverViewport(
        ImHashStr("quat_dockspace"), ImGui::GetMainViewport(),
        ImGuiDockNodeFlags_NoCloseButton | ImGuiDockNodeFlags_NoWindowMenuButton);
    if (!layout_checked_ || reset_layout) {
        // A layout loaded from the ini file has split the dockspace; keep it.
        const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace_id);
        const bool saved = node != nullptr && node->IsSplitNode();
        if (reset_layout || !saved) {
            build_default_layout(dockspace_id);
            layout_focus_frames_ = 10; // bring the first tab of each group forward
        }
        layout_checked_ = true;
        reset_layout = false;
    }
}

void EditorShell::draw_panels() {
    // Submission order is the tab order within each dock group.
    const EditorContext& c = *ctx;
    const auto open = [this](Panel p) { return &panel_open[static_cast<std::size_t>(p)]; };
    draw_viewport(*this);
    if (c.scene != nullptr && *open(Panel::inspector)) draw_inspector(*c.scene, selected);
    if (c.renderer != nullptr) draw_renderer_panel(*c.renderer, c.scene);
    if (c.scene != nullptr) {
        if (rename_pending) {
            begin_rename(hierarchy, *c.scene, selected);
            rename_pending = false;
        }
        draw_scene_hierarchy(*c.scene, selected, hierarchy, spawn_point(c.view),
                             panel_window_name(Panel::hierarchy), open(Panel::hierarchy));
    }
    if (!c.project_root.empty() && c.instantiate_request != nullptr) {
        draw_asset_browser(c.project_root, *c.instantiate_request, assets,
                           panel_window_name(Panel::assets), open(Panel::assets));
    }
    panels::draw_console(*console, panel_window_name(Panel::console), open(Panel::console));
    if (c.scene != nullptr) {
        draw_animation_preview(*c.scene, selected, c.view_proj, viewport_rect, anim_state);
        if (behavior_panel == nullptr) behavior_panel = std::make_unique<BehaviorPanel>();
        behavior_panel->draw(*c.scene, selected);
        draw_physics_debug(*c.scene, c.view_proj, viewport_rect, physics_state,
                           c.build_navmesh_request, c.nav_edges, c.nav_status);
    }
    draw_stats_panel(*this);
}

void EditorShell::draw(const EditorContext& context) {
    ctx = &context;
    last_dt_ = context.dt;
    if (has_scene() && selected != entt::null && !ctx->scene->registry().valid(selected)) {
        selected = entt::null;
    }
    dispatch_shortcuts();
    draw_main_menu(*this);
    draw_main_toolbar(*this);
    draw_status_bar(*this);
    draw_dockspace();
    draw_panels();
    draw_command_palette(*this);
    draw_dialogs(*this);

    // Toasts sit above the status bar, inside the work area.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ui::draw_toasts(last_dt_, ImVec2(vp->WorkPos.x + vp->WorkSize.x,
                                     vp->WorkPos.y + vp->WorkSize.y));

    // After a fresh layout: Assets and Inspector lead their tab groups.
    if (layout_focus_frames_ > 0) {
        --layout_focus_frames_;
        // Newly created windows take focus as they appear, and a focused
        // docked window keeps its tab in front: hand focus to the viewport.
        ImGui::SetWindowFocus(panel_window_id(Panel::viewport));
        select_docked_tab(panel_window_id(Panel::assets));
        select_docked_tab(panel_window_id(Panel::inspector));
        if (const std::optional<Panel> p = find_panel(pending_focus_)) {
            select_docked_tab(panel_window_id(*p)); // a requested panel wins
        }
    }
    // Docked windows exist after their first frame; keep asking for a few.
    if (!pending_focus_.empty()) {
        const std::optional<Panel> p = find_panel(pending_focus_);
        ImGui::SetWindowFocus(p ? panel_window_id(*p) : pending_focus_.c_str());
        if (++focus_frames_ >= 3 && layout_focus_frames_ == 0) {
            pending_focus_.clear();
            focus_frames_ = 0;
        }
    }
    ctx = nullptr;
}

} // namespace engine::editor::shell
