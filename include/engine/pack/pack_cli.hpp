#pragma once

// quat-pack's command line and driver (ROADMAP P4). The executable
// (tools/quat-pack/main.cpp) only gathers its defaults and calls run_pack.
//
//   quat-pack <project dir | game.json> --player <quat-player> --out <dir>
//   (shipping packages lay out quat-player-shipping from the same folder)
//             [--config shipping|development] [--build-id <id>]
//             [--cook auto|off] [--quat-cook <exe>] [--shaders <dir>]
//             [--engine-root <dir>] [--stage-only] [--smoke-test]
//
// Steps (each announced as "[pack] step <i>/<n> <what>" for BuildJob):
// validate the project, collect its files, stage the data root (cook,
// manifest), lay out the package for the host OS with
// scripts/package-release.sh, and optionally smoke-test it with
// scripts/smoke-test-package.sh. Ends with "[pack] folder <dir>" and
// "[pack] package <archive>", or "[pack] error: <why>" and exit code 1.

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/error.hpp"
#include "engine/project/package_manifest.hpp"

namespace engine::pack {

enum class CookMode : std::uint8_t { automatic, off };

struct PackOptions {
    std::filesystem::path project; // project directory or its game.json
    std::filesystem::path player;  // quat-player executable
    std::filesystem::path out;     // package folder + archive go here
    project::PackageConfig config = project::PackageConfig::shipping;
    std::string            build_id;
    CookMode               cook = CookMode::automatic;
    std::optional<std::filesystem::path> quat_cook;
    std::optional<std::filesystem::path> shaders;
    std::optional<std::filesystem::path> engine_root;
    bool stage_only = false;
    bool smoke_test = false;
};

// Where the defaults come from: the build tree baked into development builds
// of quat-pack, and the directory quat-pack runs from.
struct PackDefaults {
    std::optional<std::filesystem::path> shader_dir;  // cooked engine shaders
    std::optional<std::filesystem::path> engine_root; // holds scripts/ + packaging/
    std::optional<std::filesystem::path> tool_dir;    // quat-pack's own directory
    std::string                          engine_version;
};

// The engine SDK layout (ROADMAP P8, docs/sdk.md): quat-pack installed in
// <sdk>/bin finds the engine root (scripts/ + packaging/) and the cooked
// engine shaders in <sdk>/share/quat3d. Fills the defaults a development
// build did not bake in, from `tool_dir` (quat-pack's directory); leaves
// any default already set. No-op when tool_dir/../share/quat3d does not
// hold scripts/package-release.sh.
void apply_installed_sdk_defaults(PackDefaults& defaults);

// Parses argv[1..]; the error explains the usage problem.
[[nodiscard]] std::expected<PackOptions, core::Error>
parse_pack_args(std::span<const std::string> args);

[[nodiscard]] std::string_view pack_usage();

// A build id may name packages: 1-64 of [A-Za-z0-9.-], starting alphanumeric.
[[nodiscard]] std::expected<void, core::Error> validate_build_id(std::string_view id);

// The player binary a package of `config` ships, given --player (the
// development quat-player, or quat-player-shipping): shipping packages use
// quat-player-shipping next to it (remote control compiled out), development
// packages quat-player itself. The error says what to build.
[[nodiscard]] std::expected<std::filesystem::path, core::Error>
player_for_config(const std::filesystem::path& player, project::PackageConfig config);

// Runs every step; `out` receives the log (progress lines included). Returns
// the process exit code.
[[nodiscard]] int run_pack(const PackOptions& options, const PackDefaults& defaults,
                           const std::function<void(std::string_view)>& out);

} // namespace engine::pack
