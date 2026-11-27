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
using engine::core::path_from_utf8;
using engine::core::path_to_utf8;

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

} // namespace

std::expected<GameBoot, engine::core::Error> resolve_game_boot(const GameBootInputs& in) {
    namespace project = engine::project;
    fs::path root;
    std::error_code ec;
    if (in.game_override) {
        root = *in.game_override;
        if (fs::is_regular_file(root, ec)) {
            if (root.filename() != fs::path(project::k_game_project_file)) {
                return fail("QUAT_GAME='" + path_to_utf8(*in.game_override) +
                            "': expected a game project directory or its game.json");
            }
            root = root.parent_path();
            if (root.empty()) root = ".";
        }
        root = absolute_normal(root);
        if (!fs::is_regular_file(root / fs::path(project::k_game_project_file), ec)) {
            return fail("QUAT_GAME='" + path_to_utf8(*in.game_override) +
                        "': no game.json in '" + path_to_utf8(root) + "'");
        }
    } else {
        root = absolute_normal(in.data_root);
        if (!fs::is_regular_file(root / fs::path(project::k_game_project_file), ec)) {
            return fail("no game.json in the data root '" + path_to_utf8(root) +
                        "' (a game package keeps game.json next to the executable; to run a "
                        "game from its project folder set QUAT_GAME=<project dir>)");
        }
    }

    auto loaded = project::load_game_project(root);
    if (!loaded) return fail(path_to_utf8(root) + ": " + loaded.error().message);

    GameBoot boot;
    boot.game = OpenGame{root, std::move(*loaded)};
    const project::GameProject& p = boot.game.project;
    boot.start_scene = root / path_from_utf8(p.start_scene);
    if (!p.icon.empty()) boot.icon = root / path_from_utf8(p.icon);

    // A package's manifest: its config decides the remote policy.
    const fs::path manifest_path = root / fs::path(project::k_package_manifest_file);
    if (fs::is_regular_file(manifest_path, ec)) {
        const std::uintmax_t size = fs::file_size(manifest_path, ec);
        if (ec || size > k_max_manifest_bytes) return fail("manifest.json: too large");
        std::ifstream f(manifest_path, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        const nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded()) return fail("manifest.json: not valid JSON");
        auto manifest = project::parse_manifest(doc);
        if (!manifest) return std::unexpected(manifest.error());
        if (manifest->game_id != p.id) {
            return fail("manifest.json belongs to '" + manifest->game_id + "', game.json to '" +
                        p.id + "'; the package is mixed up - unpack it again");
        }
        boot.config = manifest->config;
        boot.packaged = true;
    }
    return boot;
}

void apply_game_boot(const GameBoot& boot, AppConfig& config) {
    const engine::project::GameProject& p = boot.game.project;
    config.window_title = engine::project::window_title(p);
    config.window_width = p.window.width;
    config.window_height = p.window.height;
    config.fullscreen = p.window.fullscreen;
    config.window_icon = boot.icon;
    config.start_scene = boot.start_scene;
    config.shader_hot_reload = false;
    config.remote_allowed = boot.config == engine::project::PackageConfig::development;
    config.editor_ui = false;
    config.game = boot.game;
}

} // namespace quat::app
