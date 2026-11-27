#include "engine/core/asset_root.hpp"

#include <system_error>

namespace engine::core {

namespace {

namespace fs = std::filesystem;

[[nodiscard]] bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

} // namespace

fs::path cooked_shader_subdir() {
    return fs::path("cooked") / "shaders";
}

bool is_asset_root(const fs::path& dir) {
    return !dir.empty() && is_dir(dir / "assets") && is_dir(dir / cooked_shader_subdir());
}

std::expected<AssetRoot, Error> resolve_asset_root(const AssetRootCandidates& candidates) {
    if (candidates.override_root) {
        const fs::path& root = *candidates.override_root;
        if (!is_asset_root(root)) {
            return std::unexpected(make_error(
                "QUAT_ASSET_ROOT='" + root.string() +
                "' is not a data root (expected assets/ and cooked/shaders/ inside it)"));
        }
        return AssetRoot{root, root / cooked_shader_subdir(), "QUAT_ASSET_ROOT"};
    }

    std::string tried;
    for (const fs::path& dir : candidates.install_dirs) {
        if (is_asset_root(dir)) {
            return AssetRoot{dir, dir / cooked_shader_subdir(), "install directory"};
        }
        tried += "\n  " + dir.string();
    }

    if (candidates.build_tree_root) {
        const fs::path& root = *candidates.build_tree_root;
        const fs::path shaders = candidates.build_tree_shader_dir
                                     ? *candidates.build_tree_shader_dir
                                     : root / cooked_shader_subdir();
        if (is_dir(root / "assets") && is_dir(shaders)) {
            return AssetRoot{root, shaders, "build tree"};
        }
        tried += "\n  " + root.string() + " (build tree)";
    }

    return std::unexpected(make_error(
        "no data directory (assets/ + cooked/shaders/) found; tried:" + tried +
        "\nset QUAT_ASSET_ROOT to the directory that contains them"));
}

} // namespace engine::core
