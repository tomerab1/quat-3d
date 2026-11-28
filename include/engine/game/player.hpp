#pragma once

// Entry points of a game's own executables (ROADMAP G17.1). quat_add_game()
// generates a main() that calls these with the game's modules; a game that
// writes its own main() calls them the same way.
//
//   run_player — the game's player (also its shipping player, built with
//                remote control compiled out): boots the project, checks that
//                every module game.json lists is linked, registers the modules,
//                loads the start scene and runs in play.
//   run_editor — the engine editor with the game's modules registered (their
//                components in the Inspector and Add Component, their systems
//                in Play, their methods on the remote endpoint).
//
// Each executable links the matching SDK target: quat3d::player,
// quat3d::player_shipping or quat3d::player_editor.

#include <filesystem>
#include <span>

#include "engine/game/module.hpp"

namespace engine::game {

struct PlayerOptions {
    // The project to open when QUAT_GAME is not set (development builds: the
    // game's source tree; quat_add_game passes PROJECT_DIR). Empty: the data
    // root (a packaged game keeps game.json next to the executable).
    std::filesystem::path default_project{};
};

[[nodiscard]] int run_player(int argc, char** argv, std::span<const ModuleEntry> modules,
                             const PlayerOptions& options = {});
[[nodiscard]] int run_editor(int argc, char** argv, std::span<const ModuleEntry> modules,
                             const PlayerOptions& options = {});

// Unnamed registration functions (a hand-written main()). game.json's
// "modules" cannot be matched by name then: a player with fewer functions
// than it lists refuses the game, otherwise the check is skipped with a
// warning. Prefer ModuleEntry (quat_add_game generates it).
[[nodiscard]] int run_player(int argc, char** argv, std::span<const RegisterFn> modules,
                             const PlayerOptions& options = {});

} // namespace engine::game
