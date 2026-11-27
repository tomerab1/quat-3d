#pragma once

// GameBoot — how a process finds the game it runs (ROADMAP P4, app layer).
//
// quat-player boots a game project: the directory holding game.json.
//   1. QUAT_GAME=<project dir | its game.json>, if set (development: run a
//      game from the source tree against the build's cooked shaders);
//   2. otherwise the data root (src/app/asset_root.hpp) — a game package
//      keeps game.json, manifest.json, assets/ and cooked/ next to the
//      executable.
// game.json is validated (engine/project/game_project.hpp).
//
// Remote-control policy, fail closed: a shipping build of the player
// (QUAT_SHIPPING, `shipping_build`) is always shipping and ignores QUAT_GAME.
// Otherwise the policy is the stricter of
//   - the data root's: a data root holding game.json is a package, and its
//     manifest.json decides; a missing or invalid manifest means shipping
//     (deleting the manifest cannot unlock remote control);
//   - the booted project's own manifest.json, if it has one (invalid:
//     shipping).
// So QUAT_GAME can lower the policy but never raise it above the package the
// player runs from. The editor build opens a game project the same way when
// QUAT_GAME is set, so File > Build & Export and project.* work on it.
//
// resolve_game_boot is pure logic over the inputs (tested); the entry points
// supply the environment and call open_project_root (asset_root.hpp) with the
// result.

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "app/app_config.hpp"
#include "engine/core/error.hpp"
#include "engine/project/package_manifest.hpp"

namespace quat::app {

struct GameBootInputs {
    // QUAT_GAME: a project directory, or its game.json. Relative paths are
    // taken against the current directory.
    std::optional<std::filesystem::path> game_override;
    // The resolved data root (holds assets/ + cooked/shaders/).
    std::filesystem::path data_root;
    // A shipping build of the player (QUAT_SHIPPING): game_override is
    // ignored and the config is always shipping.
    bool shipping_build = false;
};

struct GameBoot {
    OpenGame                         game;
    std::filesystem::path            start_scene; // absolute
    std::optional<std::filesystem::path> icon;    // absolute
    engine::project::PackageConfig   config = engine::project::PackageConfig::development;
    bool                             packaged = false; // runs from a game package
    // Why the policy was tightened or an input ignored (logged at boot).
    std::vector<std::string>         warnings;
};

// Finds, loads and validates the game project; the error says where it
// looked and what is wrong.
[[nodiscard]] std::expected<GameBoot, engine::core::Error>
resolve_game_boot(const GameBootInputs& inputs);

// The player's configuration for `boot`: window, icon, start scene, no
// shader hot reload, remote control only in development configs.
void apply_game_boot(const GameBoot& boot, AppConfig& config);

} // namespace quat::app
