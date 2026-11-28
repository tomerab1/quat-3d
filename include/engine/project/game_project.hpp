#pragma once

// Game project descriptor (ROADMAP P4): the `game.json` at the root of a game
// made with the engine — its name, reverse-DNS id, version, start scene,
// window, icon and the content roots shipped whole. Read by the runtime
// player (GameBoot), the packager (quat-pack), the editor's export dialog and
// the remote project.* methods. The format is documented in
// docs/game-packaging.md.
//
// game.json is untrusted input: it is read through core::JsonFields in strict
// mode (unknown keys are errors naming the known ones), every path is
// project-relative and confined to the project root, and a format newer than
// this engine reads is rejected with a message that says so.

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::project {

// The newest game.json format this engine reads (and the one it writes for
// projects that use format 2's keys). Format 1 stays valid.
inline constexpr int k_game_project_format = 2;
inline constexpr std::size_t k_max_game_modules = 64;
inline constexpr std::string_view k_game_project_file = "game.json";
inline constexpr std::size_t      k_max_game_project_bytes = 1U << 20;
inline constexpr std::size_t      k_max_content_roots = 256;

struct WindowSettings {
    std::string title; // empty: the game's name
    int         width = 1280;
    int         height = 720;
    bool        fullscreen = false;
};

struct GameProject {
    int         format = k_game_project_format;
    std::string name;    // display name
    std::string id;      // reverse-DNS, e.g. "dev.quat3d.coin-collector"
    std::string version; // MAJOR.MINOR.PATCH[-prerelease]
    // Project-relative paths ('/' separators, no "." / ".." segments).
    std::string              start_scene;
    std::string              icon; // empty: none
    std::vector<std::string> content;
    WindowSettings           window;
    // Format 2 (ROADMAP G17.1).
    // Native game modules the game needs (quat_add_game_module names); a
    // player that was not built with every one of them refuses the game.
    std::vector<std::string> modules;
    // Project-relative folder of game MCP tool descriptions and skills
    // (G17.2); empty: none.
    std::string mcp;
    // The engine versions the game was built for, e.g. ">=0.4,<0.5"; empty:
    // any. Checked when the project loads.
    std::string engine;
};

// Decodes and validates a game.json document (lexical checks only; see
// load_game_project for the on-disk checks). Errors name the field:
// "game.json: id: ...".
[[nodiscard]] std::expected<GameProject, core::Error> parse_game_project(const nlohmann::json& doc);

// Reads <project_root>/game.json, validates it, and checks its paths against
// the files on disk: the start scene and icon must be files, content roots
// must exist, and none may leave the project root (symlinks included).
[[nodiscard]] std::expected<GameProject, core::Error>
load_game_project(const std::filesystem::path& project_root);

// The project as game.json (canonical field order; defaults written out).
[[nodiscard]] nlohmann::json game_project_json(const GameProject& project);

// The window title: window.title, or the name.
[[nodiscard]] std::string window_title(const GameProject& project);

// Executable / package base name: the id's last segment, lower-cased
// ("dev.quat3d.Coin-Collector" -> "coin-collector").
[[nodiscard]] std::string executable_slug(const GameProject& project);

// The numeric MAJOR.MINOR.PATCH core of a valid version ("1.2.0-rc.1" ->
// "1.2.0"), as macOS CFBundleVersion wants it.
[[nodiscard]] std::string numeric_version(std::string_view version);

// A package output folder (absolute, or relative to `project_root`) must not
// overlap what the game ships: it may not equal, lie inside, or contain a
// content root or the start scene's folder (packages would otherwise be
// collected into the next package, or a rebuild would delete game files).
// Folders outside the project are fine. The error names both, e.g.
// "output: 'assets/dist' is inside the content root 'assets'".
[[nodiscard]] std::expected<void, core::Error>
check_output_folder(const GameProject& project, const std::filesystem::path& project_root,
                    const std::filesystem::path& output);

// Format 2's "engine" range: comma-separated comparators (>=, >, <=, <, = or
// none) on MAJOR.MINOR[.PATCH], all of which must hold: ">=0.4,<0.5".
[[nodiscard]] std::expected<void, core::Error> validate_engine_range(std::string_view range);
// Whether engine version major.minor.patch satisfies `range`; the error says
// which version the game needs and which engine this is.
[[nodiscard]] std::expected<void, core::Error> check_engine_range(std::string_view range, int major,
                                                                  int minor, int patch);

// Individual validators (also used by quat-pack's command line).
[[nodiscard]] std::expected<void, core::Error> validate_game_id(std::string_view id);
[[nodiscard]] std::expected<void, core::Error> validate_game_version(std::string_view version);
// A project-relative path as written in game.json / scene files: non-empty,
// '/'-separated, no empty / "." / ".." segment, no '\\', ':' or control
// characters, not absolute.
[[nodiscard]] std::expected<void, core::Error> validate_project_path(std::string_view path);

} // namespace engine::project
