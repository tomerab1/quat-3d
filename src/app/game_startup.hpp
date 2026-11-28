#pragma once

// Entry-point glue for game projects (ROADMAP P4): reads QUAT_GAME and the
// data root, runs GameBoot (game_boot.hpp) and opens the project root
// (asset_root.hpp). Kept out of the entry points so they stay thin.

#include <expected>

#include "app/app_config.hpp"
#include "engine/core/error.hpp"

namespace quat::app {

// quat-player: boots the game (QUAT_GAME or game.json next to the
// executable) and configures `config` for it. The error explains what is
// missing or invalid.
[[nodiscard]] std::expected<void, engine::core::Error> boot_player(AppConfig& config);

// The editor build: with QUAT_GAME set, opens that game project for editing
// and Build & Export (a bad project is logged and skipped). No-op otherwise.
void open_game_from_environment(AppConfig& config);

} // namespace quat::app
