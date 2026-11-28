#pragma once

// Starting a game package build from the running engine (ROADMAP P4/E7, app
// layer): the editor's Build & Export dialog and the remote project.build
// method both land here. quat-pack and quat-player are found next to the
// running executable (one build tree produces all three), or through
// QUAT_PACK / QUAT_PLAYER.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

#include "engine/core/error.hpp"
#include "engine/project/build_job.hpp"

namespace quat::app {

struct GameBuildSettings {
    std::string                    target; // empty: the host's
    std::filesystem::path          output; // absolute
    engine::project::PackageConfig config = engine::project::PackageConfig::shipping;
    bool                           smoke_test = false;
};

[[nodiscard]] std::expected<std::uint64_t, engine::core::Error>
start_game_build(engine::project::BuildJob& job, const std::filesystem::path& project_root,
                 const GameBuildSettings& settings);

} // namespace quat::app
