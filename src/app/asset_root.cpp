#include "app/asset_root.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <SDL3/SDL_filesystem.h>

#include "engine/core/asset_root.hpp"

namespace quat::app {

namespace {

namespace fs = std::filesystem;

struct ResolvedRoot {
    std::string project_root;
    std::string shader_dir;
};

[[nodiscard]] ResolvedRoot resolve_or_exit() {
    engine::core::AssetRootCandidates candidates;
    if (const char* env = std::getenv("QUAT_ASSET_ROOT"); env != nullptr && *env != '\0') {
        candidates.override_root = fs::path(env);
    }
    // SDL_GetBasePath: the executable's directory, or a macOS bundle's
    // Contents/Resources (where the packaged .app keeps its data).
    if (const char* base = SDL_GetBasePath(); base != nullptr) {
        candidates.install_dirs.push_back(fs::path(base).lexically_normal().parent_path());
    }
#if defined(QUAT_BUILD_TREE_ROOT)
    candidates.build_tree_root = fs::path(QUAT_BUILD_TREE_ROOT);
#endif
#if defined(QUAT_BUILD_TREE_SHADER_DIR)
    candidates.build_tree_shader_dir = fs::path(QUAT_BUILD_TREE_SHADER_DIR);
#endif

    auto root = engine::core::resolve_asset_root(candidates);
    if (!root) {
        std::fprintf(stderr, "[fatal] %s\n", root.error().message.c_str());
        std::exit(EXIT_FAILURE);
    }
    std::fprintf(stderr, "[info] data root (%s): %s\n", root->origin.c_str(),
                 root->project_root.string().c_str());
    return {root->project_root.string(), root->shader_dir.string()};
}

[[nodiscard]] const ResolvedRoot& resolved() {
    static const ResolvedRoot root = resolve_or_exit();
    return root;
}

} // namespace

const char* project_root() { return resolved().project_root.c_str(); }

const char* cooked_shader_dir() { return resolved().shader_dir.c_str(); }

} // namespace quat::app
