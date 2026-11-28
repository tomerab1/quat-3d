#pragma once

// AppConfig — the process-level options the entry points (src/main.cpp for
// the editor build, src/player/main.cpp for quat-player) hand to the
// Application. Graphics-free, so GameBoot and its tests use it directly.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "engine/project/game_project.hpp"

namespace engine::game {
class ModuleSet;
}

namespace quat::app {

// A game project the process opened (QUAT_GAME, or game.json next to the
// player): project.info / project.build and the editor's Build & Export
// dialog work on it.
struct OpenGame {
    std::filesystem::path         root; // absolute; holds game.json
    engine::project::GameProject project;
};

struct AppConfig {
    // QUAT_MAX_FRAMES: auto-quit after this many presented frames (0 = run
    // until the window closes). Headless smoke tests verify clean teardown.
    std::uint64_t max_frames = 0;
    // QUAT_SCREENSHOT (with QUAT_MAX_FRAMES): the final frame as binary PPM.
    std::optional<std::filesystem::path> screenshot_path;
    // False with QUAT_NO_UI=1: no editor shell (deterministic headless renders).
    bool editor_ui = true;

    // The window (a game's game.json sets these for the player;
    // QUAT_WINDOW_SIZE=<w>x<h> sets the initial size, e.g. for headless
    // captures of the editor).
    std::string                          window_title = "quat-3d";
    int                                  window_width = 1280;
    int                                  window_height = 720;
    bool                                 fullscreen = false;
    std::optional<std::filesystem::path> window_icon; // PNG

    // Load this scene file at startup instead of a demo scene (the player:
    // game.json's start scene).
    std::optional<std::filesystem::path> start_scene;
    // Shader hot reload (R2) as the environment asks; the player never
    // watches shader sources.
    bool shader_hot_reload = true;
    // False: QUAT_REMOTE is ignored (shipping game packages).
    bool remote_allowed = true;

    std::optional<OpenGame> game;
    // The game's native modules (G17.1), registered before the start scene
    // loads; null: none (the stock players).
    std::shared_ptr<const engine::game::ModuleSet> modules;
};

// QUAT_MAX_FRAMES, QUAT_SCREENSHOT, QUAT_NO_UI and QUAT_WINDOW_SIZE.
[[nodiscard]] AppConfig app_config_from_environment();

} // namespace quat::app
