// Game executables' entry points (ROADMAP G17.1) — see
// include/engine/game/player.hpp. Compiled into each player library: the
// editor variant (ENGINE_EDITOR) defines run_editor, the others run_player.

#include "engine/game/player.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "app/app_config.hpp"
#include "app/application.hpp"
#include "app/asset_root.hpp"
#include "app/game_startup.hpp"
#include "app/remote_bridge.hpp"
#include "engine/core/log.hpp"
#include "engine/core/stderr_capture.hpp"
#include "engine/game/module_set.hpp"
#include "engine/scene/component_registry.hpp"

namespace engine::game {

namespace {

using quat::app::AppConfig;

// The module names game.json lists must all be linked (`named`), then the
// modules register: before the start scene loads, so its module components
// decode.
std::expected<void, core::Error> attach_modules(AppConfig& config,
                                                std::span<const ModuleEntry> modules, bool named) {
    const std::vector<std::string> none;
    const std::vector<std::string>& required = config.game ? config.game->project.modules : none;
    if (named) {
        for (const std::string& name : required) {
            if (std::ranges::none_of(modules, [&](const ModuleEntry& m) { return m.name == name; })) {
                return std::unexpected(core::Error{
                    "game.json needs the native module '" + name +
                    "', which this executable was not built with (quat_add_game(... MODULES " +
                    name + "))"});
            }
        }
    } else if (!required.empty()) {
        if (modules.size() < required.size()) {
            return std::unexpected(core::Error{
                "game.json lists " + std::to_string(required.size()) + " native modules; this "
                "executable registers " + std::to_string(modules.size())});
        }
        core::log(core::LogLevel::warn, "game",
                  "modules registered without names; game.json's \"modules\" is not checked by name");
    }
    if (modules.empty()) return {};
    ModuleSet::Options options;
    options.project_root = config.game ? config.game->root : quat::app::data_root();
    auto set = ModuleSet::load(modules, scene::component_registry(), std::move(options));
    if (!set) return std::unexpected(set.error());
    std::string names;
    for (const std::string& n : set->names()) names += (names.empty() ? "" : ", ") + n;
    core::log(core::LogLevel::info, "game", "native modules: {}", names);
    config.modules = std::make_shared<const ModuleSet>(std::move(*set));
    return {};
}

// The development default project, while it exists (a binary moved to
// another machine boots from its data root instead).
[[nodiscard]] std::optional<std::filesystem::path> default_game(const PlayerOptions& options) {
    std::error_code ec;
    if (options.default_project.empty() ||
        !std::filesystem::is_regular_file(options.default_project / "game.json", ec)) {
        return std::nullopt;
    }
    return options.default_project;
}

#ifndef ENGINE_EDITOR
int player_main(std::span<const ModuleEntry> modules, bool named, const PlayerOptions& options) {
    // stderr -> engine log ring for debug.log (QUAT_REMOTE runs only).
    // Created first, destroyed last: it must outlive everything that logs.
    const core::StderrCapture stderr_capture = quat::app::capture_stderr_for_remote();
    AppConfig config = quat::app::app_config_from_environment();
    if (auto booted = quat::app::boot_player(config, default_game(options)); !booted) {
        std::fprintf(stderr, "[fatal] %s\n", booted.error().message.c_str());
        return 1;
    }
    if (auto ok = attach_modules(config, modules, named); !ok) {
        std::fprintf(stderr, "[fatal] %s\n", ok.error().message.c_str());
        return 1;
    }
    quat::app::Application app(std::move(config));
    return app.run();
}
#endif

} // namespace

#ifndef ENGINE_EDITOR
int run_player(int /*argc*/, char** /*argv*/, std::span<const ModuleEntry> modules,
               const PlayerOptions& options) {
    return player_main(modules, true, options);
}

int run_player(int /*argc*/, char** /*argv*/, std::span<const RegisterFn> modules,
               const PlayerOptions& options) {
    std::vector<std::string> names;
    std::vector<ModuleEntry> entries;
    names.reserve(modules.size());
    for (std::size_t i = 0; i < modules.size(); ++i) names.push_back("module_" + std::to_string(i));
    for (std::size_t i = 0; i < modules.size(); ++i) entries.push_back({names[i], modules[i]});
    return player_main(entries, false, options);
}
#else
int run_editor(int /*argc*/, char** /*argv*/, std::span<const ModuleEntry> modules,
               const PlayerOptions& options) {
    // A5: stderr -> engine log ring for debug.log and the editor Console.
    const bool editor_console = std::getenv("QUAT_NO_UI") == nullptr;
    const core::StderrCapture stderr_capture = quat::app::capture_stderr_for_remote(editor_console);
    AppConfig config = quat::app::app_config_from_environment();
    quat::app::open_game_from_environment(config, default_game(options));
    if (auto ok = attach_modules(config, modules, true); !ok) {
        std::fprintf(stderr, "[fatal] %s\n", ok.error().message.c_str());
        return 1;
    }
    quat::app::Application app(std::move(config));
    return app.run();
}
#endif

} // namespace engine::game
