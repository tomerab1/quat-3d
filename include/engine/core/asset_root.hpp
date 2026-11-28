#pragma once

// Runtime resolution of the read-only data root: the directory holding
// `assets/` and `cooked/shaders/`. A development build runs from the source
// tree; a packaged build (release archive, macOS .app) carries its data next
// to the executable, wherever it was unpacked. Pure logic over candidate
// directories — the caller supplies them (executable dir, env override, the
// build tree), so it is testable and platform-free.

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/error.hpp"

namespace engine::core {

struct AssetRoot {
    std::filesystem::path project_root; // holds assets/
    std::filesystem::path shader_dir;   // cooked SPIR-V + reflection JSON
    std::string           origin;       // which candidate won (for logs)
};

struct AssetRootCandidates {
    // Explicit override (QUAT_ASSET_ROOT). When set it must be a valid root:
    // a wrong override is an error, never silently replaced by another root.
    std::optional<std::filesystem::path> override_root;
    // Install locations, in priority order: the executable's directory, a
    // macOS bundle's Contents/Resources, ...
    std::vector<std::filesystem::path> install_dirs;
    // Development fallback: the source tree and its cooked-shader directory
    // baked in at compile time. Absent in packaged builds.
    std::optional<std::filesystem::path> build_tree_root;
    std::optional<std::filesystem::path> build_tree_shader_dir;
};

// Relative location of the cooked shaders under a root.
[[nodiscard]] std::filesystem::path cooked_shader_subdir();

// The install candidates for an executable in `exe_dir`, in priority order:
// the directory itself (packaged games: data next to the executable), then
// <exe_dir>/../share/quat3d (the engine SDK, ROADMAP P8: bin/quat-player
// next to share/quat3d's assets/ + cooked/shaders/).
[[nodiscard]] std::vector<std::filesystem::path> install_dirs_for(const std::filesystem::path& exe_dir);

// True when `dir` has the packaged layout: assets/ and cooked/shaders/.
[[nodiscard]] bool is_asset_root(const std::filesystem::path& dir);

// First valid candidate: override, then install dirs, then the build tree.
// The error lists every location tried.
[[nodiscard]] std::expected<AssetRoot, Error> resolve_asset_root(
    const AssetRootCandidates& candidates);

} // namespace engine::core
