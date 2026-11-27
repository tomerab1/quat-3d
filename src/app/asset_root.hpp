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
// main.cpp still spells these as QUAT_PROJECT_ROOT / QUAT_COOKED_SHADER_DIR;
// CMake maps those macros onto the functions below until main.cpp calls them
// directly (see CMakeLists.txt, `game`).

namespace quat::app {

// Directory holding assets/ (UTF-8/native narrow path, no trailing slash).
[[nodiscard]] const char* project_root();

// Directory holding the cooked shaders (.spv + reflection .json).
[[nodiscard]] const char* cooked_shader_dir();

} // namespace quat::app
