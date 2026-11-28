#pragma once

// The window icon from a game's PNG (ROADMAP P4: game.json "icon"). SDL
// shows it in the title bar / task bar / dock while the game runs; packaged
// macOS bundles also carry it as their .icns.

#include <expected>
#include <filesystem>

#include "engine/core/error.hpp"

struct SDL_Window;

namespace quat::app {

[[nodiscard]] std::expected<void, engine::core::Error>
set_window_icon(SDL_Window* window, const std::filesystem::path& png);

} // namespace quat::app
