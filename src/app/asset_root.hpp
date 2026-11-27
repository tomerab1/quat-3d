#pragma once

// Where the `game` executable finds its data (app layer). Resolved once, on
// first use, through engine::core::resolve_asset_root:
//   1. QUAT_ASSET_ROOT, if set (must contain assets/ and cooked/shaders/);
//   2. the executable's directory — SDL_GetBasePath(), which inside a macOS
//      .app is Contents/Resources — i.e. an unpacked release archive;
//   3. development builds only: the source tree baked in at compile time
//      (QUAT_BUILD_TREE_ROOT). Packaged builds (ENGINE_PACKAGE_BUILD=ON) embed
//      no build-machine paths.
// If nothing matches, the process prints what it tried and exits.
//
// Every game-side data path (fonts, audio clips, scene files, shader sources
// for hot reload, the remote API's project root) goes through these. They are
// std::filesystem::path so non-ASCII install directories survive on Windows;
// convert with engine/core/utf8_path.hpp where a UTF-8 string is needed (SDL,
// ImGui, pipeline-cache keys) — never with .string().

#include <filesystem>

namespace quat::app {

// Directory holding assets/.
[[nodiscard]] const std::filesystem::path& project_root();

// Directory holding the cooked shaders (.spv + reflection .json).
[[nodiscard]] const std::filesystem::path& cooked_shader_dir();

} // namespace quat::app
