// quat-player: the runtime player for games made with the engine (ROADMAP
// P4). The same engine and application layer as `game`, built without the
// editor: it boots game.json's start scene (src/app/game_startup.hpp) and
// runs it. Remote control is opt-in (QUAT_REMOTE) and only in development
// packages; shader hot reload is off. See docs/game-packaging.md.
//
// On Windows it is a GUI-subsystem program (no console window opens when a
// player double-clicks it); a terminal that redirects its output still gets
// the log through the inherited standard handles.

#include <span>

#include "engine/game/player.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// The stock player runs data-plus-Lua games: no native modules (G17.1 games
// get their own player from quat_add_game, over the same library).
#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, PWSTR /*command_line*/,
                    int /*show*/) {
    return engine::game::run_player(0, nullptr, std::span<const engine::game::ModuleEntry>{});
}
#else
int main(int argc, char** argv) {
    return engine::game::run_player(argc, argv, std::span<const engine::game::ModuleEntry>{});
}
#endif
