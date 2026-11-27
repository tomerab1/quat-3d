// The sample game (games/coin-collector) stays a valid, packageable project:
// its game.json validates and its files are found by following references
// (the coin script only through the game script's spawn literal).

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "engine/core/utf8_path.hpp"
#include "engine/pack/dependencies.hpp"
#include "engine/project/game_project.hpp"
#include "support/expect_ok.hpp"

namespace {

TEST(SampleGame, CoinCollectorPackages) {
    const auto root = engine::core::path_from_utf8(QUAT_SOURCE_DIR) / "games" / "coin-collector";
    auto project = engine::project::load_game_project(root);
    ASSERT_OK(project);
    EXPECT_EQ(engine::project::executable_slug(*project), "coin-collector");
    auto files = engine::pack::collect_game_files(root, *project);
    ASSERT_OK(files);
    std::vector<std::string> paths;
    for (const auto& f : files->files) paths.push_back(f.path);
    const std::vector<std::string> want{
        "assets/icons/coin-collector.png",
        "assets/scenes/coin_collector.json",
        "assets/scripts/coin_collector/coin.lua",
        "assets/scripts/coin_collector/follow_camera.lua",
        "assets/scripts/coin_collector/game.lua",
        "assets/scripts/coin_collector/player.lua",
        "game.json",
    };
    EXPECT_EQ(paths, want);
    EXPECT_TRUE(files->warnings.empty()) << files->warnings.front();
}

} // namespace
