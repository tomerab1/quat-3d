#include "app/asset_root.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include "engine/core/asset_root.hpp"
#include "engine/core/utf8_path.hpp"

namespace quat::app {

namespace {

namespace fs = std::filesystem;
using engine::core::path_from_utf8;
using engine::core::path_to_utf8;

[[nodiscard]] engine::core::AssetRoot resolve_or_exit() {
    engine::core::AssetRootCandidates candidates;
    // SDL_getenv / SDL_GetBasePath return UTF-8 on every platform (std::getenv
    // would give the ANSI code page on Windows).
    if (const char* env = SDL_getenv("QUAT_ASSET_ROOT"); env != nullptr && *env != '\0') {
        candidates.override_root = path_from_utf8(env);
    }
    // The executable's directory, or a macOS bundle's Contents/Resources
    // (where the packaged .app keeps its data).
    if (const char* base = SDL_GetBasePath(); base != nullptr) {
        candidates.install_dirs.push_back(path_from_utf8(base).lexically_normal().parent_path());
    }
#if defined(QUAT_BUILD_TREE_ROOT)
    candidates.build_tree_root = path_from_utf8(QUAT_BUILD_TREE_ROOT);
#endif
#if defined(QUAT_BUILD_TREE_SHADER_DIR)
    candidates.build_tree_shader_dir = path_from_utf8(QUAT_BUILD_TREE_SHADER_DIR);
#endif

    auto root = engine::core::resolve_asset_root(candidates);
    if (!root) {
        std::fprintf(stderr, "[fatal] %s\n", root.error().message.c_str());
        std::exit(EXIT_FAILURE);
    }
    std::fprintf(stderr, "[info] data root (%s): %s\n", root->origin.c_str(),
                 path_to_utf8(root->project_root).c_str());
    return std::move(*root);
}

[[nodiscard]] const engine::core::AssetRoot& resolved() {
    static const engine::core::AssetRoot root = resolve_or_exit();
    return root;
}

} // namespace

const std::filesystem::path& project_root() { return resolved().project_root; }

const std::filesystem::path& cooked_shader_dir() { return resolved().shader_dir; }

} // namespace quat::app
