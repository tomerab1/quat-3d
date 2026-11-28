#include "app/game_boot.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/utf8_path.hpp"
#include "engine/project/game_project.hpp"

namespace quat::app {

namespace {

namespace fs = std::filesystem;
namespace project = engine::project;
using engine::core::path_from_utf8;
using engine::core::path_to_utf8;
using project::PackageConfig;

constexpr std::uintmax_t k_max_manifest_bytes = 64ULL << 20;

[[nodiscard]] std::unexpected<engine::core::Error> fail(std::string what) {
    return std::unexpected(engine::core::Error{std::move(what)});
}

[[nodiscard]] fs::path absolute_normal(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    abs = abs.lexically_normal();
    if (!abs.has_filename() && abs.has_parent_path()) abs = abs.parent_path(); // "dir/"
    return abs;
}

[[nodiscard]] bool has_file(const fs::path& dir, std::string_view name) {
    std::error_code ec;
    return fs::is_regular_file(dir / fs::path(name), ec);
}

[[nodiscard]] PackageConfig stricter(PackageConfig a, PackageConfig b) {
    return a == PackageConfig::shipping || b == PackageConfig::shipping ? PackageConfig::shipping
                                                                        : PackageConfig::development;
}

// manifest.json of `dir`, decoded; nullopt when absent. An unreadable or
// invalid manifest is an error (the caller fails closed on it).
[[nodiscard]] std::expected<std::optional<project::PackageManifest>, engine::core::Error>
read_manifest(const fs::path& dir) {
    const fs::path path = dir / fs::path(project::k_package_manifest_file);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return std::optional<project::PackageManifest>{};
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > k_max_manifest_bytes) return fail("manifest.json: unreadable or too large");
    std::ifstream f(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    const nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) return fail("manifest.json: not valid JSON");
    auto manifest = project::parse_manifest(doc);
    if (!manifest) return std::unexpected(manifest.error());
    return std::optional<project::PackageManifest>(std::move(*manifest));
}

// The policy a package root allows: its manifest's config, shipping when
// the manifest is missing or invalid (a package without its manifest is not
// trusted to be a development build).
[[nodiscard]] PackageConfig package_policy(const fs::path& root, std::vector<std::string>& warnings) {
    auto manifest = read_manifest(root);
    if (!manifest) {
        warnings.push_back(manifest.error().message + " - treated as a shipping package");
        return PackageConfig::shipping;
    }
    if (!*manifest) {
        warnings.push_back("no manifest.json next to game.json in '" + path_to_utf8(root) +
                           "' - treated as a shipping package");
        return PackageConfig::shipping;
    }
    return (*manifest)->config;
}

} // namespace

std::expected<GameBoot, engine::core::Error> resolve_game_boot(const GameBootInputs& in) {
    GameBoot boot;
    const fs::path data_root = absolute_normal(in.data_root);
    std::optional<fs::path> game_override = in.game_override;
    if (in.shipping_build && game_override) {
        boot.warnings.push_back("QUAT_GAME is ignored by a shipping player");
        game_override.reset();
    }

    fs::path root;
    std::error_code ec;
    if (game_override) {
        root = *game_override;
        if (fs::is_regular_file(root, ec)) {
            if (root.filename() != fs::path(project::k_game_project_file)) {
                return fail("QUAT_GAME='" + path_to_utf8(*game_override) +
                            "': expected a game project directory or its game.json");
            }
            root = root.parent_path();
            if (root.empty()) root = ".";
        }
        root = absolute_normal(root);
        if (!has_file(root, project::k_game_project_file)) {
            return fail("QUAT_GAME='" + path_to_utf8(*game_override) + "': no game.json in '" +
                        path_to_utf8(root) + "'");
        }
    } else {
        root = data_root;
        if (!has_file(root, project::k_game_project_file)) {
            return fail("no game.json in the data root '" + path_to_utf8(root) +
                        "' (a game package keeps game.json next to the executable; to run a "
                        "game from its project folder set QUAT_GAME=<project dir>)");
        }
    }

    auto loaded = project::load_game_project(root);
    if (!loaded) return fail(path_to_utf8(root) + ": " + loaded.error().message);
    boot.game = OpenGame{root, std::move(*loaded)};
    const project::GameProject& p = boot.game.project;
    boot.start_scene = root / path_from_utf8(p.start_scene);
    if (!p.icon.empty()) boot.icon = root / path_from_utf8(p.icon);

    // The booted project's own manifest: a package's identity must match.
    auto own = read_manifest(root);
    if (own && *own && (*own)->game_id != p.id) {
        return fail("manifest.json belongs to '" + (*own)->game_id + "', game.json to '" + p.id +
                    "'; the package is mixed up - unpack it again");
    }

    // Remote-control policy, fail closed (see the header).
    PackageConfig config = PackageConfig::development;
    if (has_file(data_root, project::k_game_project_file)) { // the player runs from a package
        config = package_policy(data_root, boot.warnings);
        boot.packaged = true;
    }
    if (root != data_root) {
        if (!own) {
            boot.warnings.push_back(own.error().message + " - treated as shipping");
            config = PackageConfig::shipping;
        } else if (*own) {
            config = stricter(config, (*own)->config);
            boot.packaged = true;
        }
    }
    if (in.shipping_build) config = PackageConfig::shipping;
    boot.config = config;
    return boot;
}

void apply_game_boot(const GameBoot& boot, AppConfig& config) {
    const project::GameProject& p = boot.game.project;
    config.window_title = project::window_title(p);
    config.window_width = p.window.width;
    config.window_height = p.window.height;
    config.fullscreen = p.window.fullscreen;
    config.window_icon = boot.icon;
    config.start_scene = boot.start_scene;
    config.shader_hot_reload = false;
    config.remote_allowed = boot.config == PackageConfig::development;
    config.editor_ui = false;
    config.game = boot.game;
}

} // namespace quat::app
