// GameBoot (ROADMAP P4): how quat-player finds and validates the game it
// runs — QUAT_GAME or game.json in the data root — and what it configures.

#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app/game_boot.hpp"
#include "engine/project/package_manifest.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_project.hpp"

namespace {

namespace fs = std::filesystem;
namespace project = engine::project;
using quat::app::GameBootInputs;
using quat::app::resolve_game_boot;

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

void write_game(const quat::test::TempProject& dir, const std::string& prefix = "") {
    dir.write(prefix + "game.json",
              quat::test::game_json("assets/scenes/main.json",
                                    R"("icon": "assets/icon.png", "window": {"width": 800, "height": 600})"));
    dir.write(prefix + "assets/scenes/main.json", R"({"entities": []})");
    dir.write(prefix + "assets/icon.png", "png");
}

void write_manifest(const quat::test::TempProject& dir, project::PackageConfig config,
                    const std::string& id = "dev.quat3d.test-game") {
    project::PackageManifest m;
    m.game_id = id;
    m.config = config;
    dir.write("manifest.json", project::manifest_json(m).dump());
}

TEST(GameBoot, PackagedGameNextToTheExecutable) {
    quat::test::TempProject data("quat_boot_pkg");
    write_game(data);
    write_manifest(data, project::PackageConfig::shipping);
    auto boot = resolve_game_boot({.game_override = std::nullopt, .data_root = data.path()});
    ASSERT_OK(boot);
    EXPECT_TRUE(boot->packaged);
    EXPECT_EQ(boot->config, project::PackageConfig::shipping);
    EXPECT_EQ(boot->game.project.name, "Test Game");
    EXPECT_TRUE(boot->game.root.is_absolute());
    EXPECT_EQ(boot->start_scene, boot->game.root / "assets" / "scenes" / "main.json");
    ASSERT_TRUE(boot->icon.has_value());

    quat::app::AppConfig config;
    quat::app::apply_game_boot(*boot, config);
    EXPECT_EQ(config.window_title, "Test Game");
    EXPECT_EQ(config.window_width, 800);
    EXPECT_EQ(config.window_height, 600);
    EXPECT_EQ(config.start_scene, boot->start_scene);
    EXPECT_EQ(config.window_icon, boot->icon);
    EXPECT_FALSE(config.shader_hot_reload);
    EXPECT_FALSE(config.remote_allowed); // shipping
    EXPECT_FALSE(config.editor_ui);
    ASSERT_TRUE(config.game.has_value());
    EXPECT_EQ(config.game->project.id, "dev.quat3d.test-game");
}

TEST(GameBoot, DevelopmentPackagesAllowRemoteControl) {
    quat::test::TempProject data("quat_boot_dev");
    write_game(data);
    write_manifest(data, project::PackageConfig::development);
    auto boot = resolve_game_boot({.game_override = std::nullopt, .data_root = data.path()});
    ASSERT_OK(boot);
    quat::app::AppConfig config;
    quat::app::apply_game_boot(*boot, config);
    EXPECT_TRUE(config.remote_allowed);
}

TEST(GameBoot, QuatGameOverridesTheDataRoot) {
    quat::test::TempProject engine_root("quat_boot_engine");
    quat::test::TempProject game("quat_boot_game");
    write_game(game, "nested/");
    // A directory...
    auto boot = resolve_game_boot({.game_override = game.path() / "nested",
                                   .data_root = engine_root.path()});
    ASSERT_OK(boot);
    EXPECT_FALSE(boot->packaged);
    EXPECT_EQ(boot->config, project::PackageConfig::development); // no manifest: a dev run
    EXPECT_EQ(boot->game.root, (game.path() / "nested").lexically_normal());
    // ...or its game.json, with a trailing separator tolerated on the directory.
    auto by_file = resolve_game_boot({.game_override = game.path() / "nested" / "game.json",
                                      .data_root = engine_root.path()});
    ASSERT_OK(by_file);
    EXPECT_EQ(by_file->game.root, boot->game.root);
    auto slash = resolve_game_boot({.game_override = fs::path(game.path() / "nested") += "/",
                                    .data_root = engine_root.path()});
    ASSERT_OK(slash);
    EXPECT_EQ(slash->game.root, boot->game.root);
}

TEST(GameBoot, ErrorsSayWhereAndWhat) {
    quat::test::TempProject data("quat_boot_err");
    auto e = resolve_game_boot({.game_override = std::nullopt, .data_root = data.path()});
    ASSERT_ERR(e);
    EXPECT_TRUE(contains(e.error().message, "no game.json in the data root")) << e.error().message;
    EXPECT_TRUE(contains(e.error().message, "QUAT_GAME")) << e.error().message;

    e = resolve_game_boot({.game_override = data.path() / "nope", .data_root = data.path()});
    ASSERT_ERR(e);
    EXPECT_TRUE(contains(e.error().message, "QUAT_GAME='")) << e.error().message;

    data.write("other.json", "{}");
    e = resolve_game_boot({.game_override = data.path() / "other.json", .data_root = data.path()});
    ASSERT_ERR(e);
    EXPECT_TRUE(contains(e.error().message, "expected a game project directory or its game.json"));

    // An invalid game.json reports the field.
    data.write("game.json", R"({"format": 1, "name": "X", "id": "bad", "version": "1.0.0",
                                "start_scene": "a.json"})");
    e = resolve_game_boot({.game_override = std::nullopt, .data_root = data.path()});
    ASSERT_ERR(e);
    EXPECT_TRUE(contains(e.error().message, "id:")) << e.error().message;

    // A manifest from another game: the package is mixed up.
    write_game(data);
    write_manifest(data, project::PackageConfig::shipping, "dev.quat3d.other");
    e = resolve_game_boot({.game_override = std::nullopt, .data_root = data.path()});
    ASSERT_ERR(e);
    EXPECT_TRUE(contains(e.error().message, "mixed up")) << e.error().message;

}

// ---- Remote-control policy: fail closed ------------------------------------

bool remote_allowed(const quat::app::GameBoot& boot) {
    quat::app::AppConfig config;
    quat::app::apply_game_boot(boot, config);
    return config.remote_allowed;
}

TEST(GameBootPolicy, PackagedRootWithoutManifestIsShipping) {
    quat::test::TempProject data("quat_boot_policy");
    write_game(data); // game.json next to the executable, manifest deleted
    auto boot = resolve_game_boot({.game_override = std::nullopt, .data_root = data.path()});
    ASSERT_OK(boot);
    EXPECT_EQ(boot->config, project::PackageConfig::shipping);
    EXPECT_TRUE(boot->packaged);
    EXPECT_FALSE(remote_allowed(*boot));
    ASSERT_FALSE(boot->warnings.empty());
    EXPECT_TRUE(contains(boot->warnings[0], "no manifest.json")) << boot->warnings[0];
}

TEST(GameBootPolicy, InvalidManifestIsShipping) {
    quat::test::TempProject data("quat_boot_policy");
    write_game(data);
    data.write("manifest.json", "{broken");
    auto boot = resolve_game_boot({.game_override = std::nullopt, .data_root = data.path()});
    ASSERT_OK(boot);
    EXPECT_EQ(boot->config, project::PackageConfig::shipping);
    ASSERT_FALSE(boot->warnings.empty());
    EXPECT_TRUE(contains(boot->warnings[0], "manifest.json")) << boot->warnings[0];

    // A manifest that is valid JSON but not a manifest: shipping too.
    data.write("manifest.json", R"({"format": 1, "config": "development"})");
    boot = resolve_game_boot({.game_override = std::nullopt, .data_root = data.path()});
    ASSERT_OK(boot);
    EXPECT_EQ(boot->config, project::PackageConfig::shipping);
}

TEST(GameBootPolicy, QuatGameCannotUnlockAShippingPackage) {
    quat::test::TempProject package("quat_boot_shipping_pkg");
    write_game(package);
    write_manifest(package, project::PackageConfig::shipping);
    quat::test::TempProject project_dir("quat_boot_dev_project");
    write_game(project_dir); // a project folder: no manifest, "development" on its own

    auto boot = resolve_game_boot({.game_override = project_dir.path(), .data_root = package.path()});
    ASSERT_OK(boot);
    EXPECT_EQ(boot->game.root, project_dir.path().lexically_normal());
    EXPECT_EQ(boot->config, project::PackageConfig::shipping);
    EXPECT_FALSE(remote_allowed(*boot));

    // Deleting the package's manifest does not help either.
    std::filesystem::remove(package.path() / "manifest.json");
    boot = resolve_game_boot({.game_override = project_dir.path(), .data_root = package.path()});
    ASSERT_OK(boot);
    EXPECT_FALSE(remote_allowed(*boot));

    // Nor a QUAT_GAME project whose own manifest claims development.
    write_manifest(project_dir, project::PackageConfig::development);
    boot = resolve_game_boot({.game_override = project_dir.path(), .data_root = package.path()});
    ASSERT_OK(boot);
    EXPECT_FALSE(remote_allowed(*boot));
}

TEST(GameBootPolicy, QuatGameCanLowerButNotRaise) {
    // A development package running a QUAT_GAME project that ships: shipping.
    quat::test::TempProject package("quat_boot_dev_pkg");
    write_game(package);
    write_manifest(package, project::PackageConfig::development);
    quat::test::TempProject other("quat_boot_other");
    write_game(other);
    write_manifest(other, project::PackageConfig::shipping);
    auto boot = resolve_game_boot({.game_override = other.path(), .data_root = package.path()});
    ASSERT_OK(boot);
    EXPECT_EQ(boot->config, project::PackageConfig::shipping);
}

TEST(GameBootPolicy, ShippingBuildIgnoresQuatGameAndManifest) {
    quat::test::TempProject package("quat_boot_ship_build");
    write_game(package);
    write_manifest(package, project::PackageConfig::development);
    quat::test::TempProject project_dir("quat_boot_dev_project");
    write_game(project_dir, "x/");
    auto boot = resolve_game_boot({.game_override = project_dir.path() / "x",
                                   .data_root = package.path(),
                                   .shipping_build = true});
    ASSERT_OK(boot);
    EXPECT_EQ(boot->game.root, package.path().lexically_normal()); // QUAT_GAME ignored
    EXPECT_EQ(boot->config, project::PackageConfig::shipping);    // manifest ignored
    EXPECT_FALSE(remote_allowed(*boot));
    ASSERT_FALSE(boot->warnings.empty());
    EXPECT_TRUE(contains(boot->warnings[0], "QUAT_GAME is ignored")) << boot->warnings[0];
}

} // namespace
