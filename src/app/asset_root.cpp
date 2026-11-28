#include "app/asset_root.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <SDL3/SDL_filesystem.h>

#include "engine/core/asset_root.hpp"
#include "engine/core/env.hpp"
#include "engine/core/utf8_path.hpp"

namespace quat::app {

namespace {

namespace fs = std::filesystem;
using engine::core::path_from_utf8;
using engine::core::path_to_utf8;

[[nodiscard]] engine::core::AssetRoot resolve_or_exit() {
    engine::core::AssetRootCandidates candidates;
    // env_path reads the UTF-16 environment on Windows (std::getenv would give
    // the ANSI code page); SDL_GetBasePath returns UTF-8.
    candidates.override_root = engine::core::env_path("QUAT_ASSET_ROOT");
    // The executable's directory, or a macOS bundle's Contents/Resources
    // (where the packaged .app keeps its data); then, for the players an
    // engine SDK installs in <sdk>/bin, <sdk>/share/quat3d (P8).
    if (const char* base = SDL_GetBasePath(); base != nullptr) {
        candidates.install_dirs =
            engine::core::install_dirs_for(path_from_utf8(base).lexically_normal().parent_path());
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
    static const engine::core::AssetRoot root = [] {
        engine::core::AssetRoot r = resolve_or_exit();
        // Absolute: the working directory may change (open_project_root).
        std::error_code ec;
        r.project_root = fs::absolute(r.project_root, ec).lexically_normal();
        r.shader_dir = fs::absolute(r.shader_dir, ec).lexically_normal();
        return r;
    }();
    return root;
}

// The opened game project, if any (set once, before readers run).
std::optional<fs::path>& opened_project() {
    static std::optional<fs::path> root;
    return root;
}

} // namespace

const std::filesystem::path& project_root() {
    const std::optional<fs::path>& opened = opened_project();
    return opened ? *opened : resolved().project_root;
}

std::expected<void, engine::core::Error> open_project_root(const std::filesystem::path& root) {
    (void)resolved(); // the data root, against the original working directory
    std::error_code ec;
    const fs::path abs = fs::absolute(root, ec).lexically_normal();
    if (ec) return std::unexpected(engine::core::Error{"cannot resolve '" + path_to_utf8(root) + "'"});
    fs::current_path(abs, ec);
    if (ec) {
        return std::unexpected(engine::core::Error{"cannot enter the game project '" +
                                                   path_to_utf8(abs) + "': " + ec.message()});
    }
    opened_project() = abs;
    std::fprintf(stderr, "[info] game project: %s\n", path_to_utf8(abs).c_str());
    return {};
}

const std::filesystem::path& data_root() { return resolved().project_root; }

const std::filesystem::path& cooked_shader_dir() { return resolved().shader_dir; }

} // namespace quat::app
