#pragma once

// Staging a game's data root (ROADMAP P4): copies the collected files
// (dependencies.hpp) into a fresh directory in the runtime layout, cooks the
// glTF models with quat-cook when one is given (R17: KTX2/Basis textures,
// meshoptimizer meshes), rewrites absolute in-project scene paths, adds the
// engine's cooked shaders and writes manifest.json. The result is what the
// player expects next to itself: game.json, manifest.json, assets/...,
// cooked/shaders/. Deterministic: same inputs, same bytes.

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "engine/core/error.hpp"
#include "engine/pack/dependencies.hpp"
#include "engine/project/game_project.hpp"
#include "engine/project/package_manifest.hpp"

namespace engine::pack {

struct StageOptions {
    std::filesystem::path project_root;
    project::GameProject  project;
    GameFiles             files;
    // Created by stage_game_data; it must not exist yet (the caller owns
    // clearing an old one).
    std::filesystem::path data_dir;
    // The engine's cooked shaders (.spv + reflection .json).
    std::filesystem::path shader_dir;
    // quat-cook executable: cook models when set.
    std::optional<std::filesystem::path> quat_cook;

    project::PackageConfig config = project::PackageConfig::shipping;
    std::string            target;
    std::string            build_id;
    std::string            engine_version;

    // Receives one line per notable action (optional).
    std::function<void(std::string_view line)> log;
};

[[nodiscard]] std::expected<project::PackageManifest, core::Error>
stage_game_data(const StageOptions& options);

} // namespace engine::pack
