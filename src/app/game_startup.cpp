#include "app/game_startup.hpp"

#include <cstdio>
#include <string>

#include "app/asset_root.hpp"
#include "app/game_boot.hpp"
#include "engine/core/env.hpp"
#include "engine/core/utf8_path.hpp"

namespace quat::app {

std::expected<void, engine::core::Error> boot_player(AppConfig& config) {
    GameBootInputs inputs;
    inputs.game_override = engine::core::env_path("QUAT_GAME");
    inputs.data_root = data_root(); // exits with a [fatal] line when there is none
    auto boot = resolve_game_boot(inputs);
    if (!boot) return std::unexpected(boot.error());
    if (auto ok = open_project_root(boot->game.root); !ok) return ok;
    apply_game_boot(*boot, config);
    const engine::project::GameProject& p = boot->game.project;
    std::fprintf(stderr, "[game] %s %s (%s, %s%s)\n", p.name.c_str(), p.version.c_str(),
                 p.id.c_str(), std::string(engine::project::to_string(boot->config)).c_str(),
                 boot->packaged ? ", packaged" : "");
    return {};
}

void open_game_from_environment(AppConfig& config) {
    const auto game = engine::core::env_path("QUAT_GAME");
    if (!game) return;
    GameBootInputs inputs;
    inputs.game_override = game;
    inputs.data_root = data_root();
    auto boot = resolve_game_boot(inputs);
    if (!boot) {
        std::fprintf(stderr, "[warn] QUAT_GAME ignored: %s\n", boot.error().message.c_str());
        return;
    }
    if (auto ok = open_project_root(boot->game.root); !ok) {
        std::fprintf(stderr, "[warn] QUAT_GAME ignored: %s\n", ok.error().message.c_str());
        return;
    }
    config.game = boot->game;
}

} // namespace quat::app
