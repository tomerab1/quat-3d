// game.json (ROADMAP P4): strict validation with precise, version-checked
// errors, on-disk path checks, and the derived names.

#include <filesystem>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/project/game_project.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_project.hpp"

namespace {

using nlohmann::json;
namespace project = engine::project;

json valid() {
    return json::parse(R"({
        "format": 1,
        "name": "Coin Collector",
        "id": "dev.quat3d.Coin-Collector",
        "version": "0.1.0",
        "start_scene": "assets/scenes/coin_collector.json",
        "window": {"title": "Coins!", "width": 1600, "height": 900, "fullscreen": true},
        "icon": "assets/icons/coin.png",
        "content": ["assets/scripts", "assets/audio/music.ogg"]
    })");
}

// The error message of parsing `doc` (empty when it parsed).
std::string error_of(const json& doc) {
    auto p = project::parse_game_project(doc);
    return p ? std::string() : p.error().message;
}

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

TEST(GameProject, ParsesAValidProject) {
    auto p = project::parse_game_project(valid());
    ASSERT_OK(p);
    EXPECT_EQ(p->name, "Coin Collector");
    EXPECT_EQ(p->id, "dev.quat3d.Coin-Collector");
    EXPECT_EQ(p->version, "0.1.0");
    EXPECT_EQ(p->start_scene, "assets/scenes/coin_collector.json");
    EXPECT_EQ(p->window.title, "Coins!");
    EXPECT_EQ(p->window.width, 1600);
    EXPECT_EQ(p->window.height, 900);
    EXPECT_TRUE(p->window.fullscreen);
    EXPECT_EQ(p->icon, "assets/icons/coin.png");
    ASSERT_EQ(p->content.size(), 2U);
    EXPECT_EQ(project::executable_slug(*p), "coin-collector");
    EXPECT_EQ(project::window_title(*p), "Coins!");
}

TEST(GameProject, DefaultsForOptionalFields) {
    json doc = valid();
    doc.erase("window");
    doc.erase("icon");
    doc.erase("content");
    auto p = project::parse_game_project(doc);
    ASSERT_OK(p);
    EXPECT_EQ(p->window.width, 1280);
    EXPECT_EQ(p->window.height, 720);
    EXPECT_FALSE(p->window.fullscreen);
    EXPECT_EQ(project::window_title(*p), "Coin Collector"); // the name
    EXPECT_TRUE(p->icon.empty());
    EXPECT_TRUE(p->content.empty());
}

TEST(GameProject, FormatIsVersionChecked) {
    json doc = valid();
    doc["format"] = 3;
    std::string e = error_of(doc);
    EXPECT_TRUE(contains(e, "format 3 is newer than this engine reads (2)")) << e;
    EXPECT_TRUE(contains(e, "update the engine")) << e;
    // A newer file fails on its format, not on the fields it added.
    doc["new_field"] = true;
    EXPECT_TRUE(contains(error_of(doc), "newer than this engine")) << error_of(doc);

    doc = valid();
    doc.erase("format");
    EXPECT_TRUE(contains(error_of(doc), "format: required")) << error_of(doc);
    doc["format"] = "1";
    EXPECT_TRUE(contains(error_of(doc), "format: expected a positive integer")) << error_of(doc);
    doc["format"] = 0;
    EXPECT_FALSE(error_of(doc).empty());
}

TEST(GameProject, UnknownAndMissingFieldsAreNamed) {
    json doc = valid();
    doc["startScene"] = "x.json";
    std::string e = error_of(doc);
    EXPECT_TRUE(contains(e, "game.json: startScene: unknown field")) << e;
    EXPECT_TRUE(contains(e, "start_scene")) << e; // listed among the known fields

    for (const char* key : {"name", "id", "version", "start_scene"}) {
        doc = valid();
        doc.erase(key);
        e = error_of(doc);
        EXPECT_TRUE(contains(e, std::string(key) + ": required")) << key << ": " << e;
    }
    doc = valid();
    doc["window"]["depth"] = 32;
    e = error_of(doc);
    EXPECT_TRUE(contains(e, "window.depth: unknown field")) << e;
    doc = valid();
    doc["window"]["width"] = 100;
    e = error_of(doc);
    EXPECT_TRUE(contains(e, "window.width")) << e;
    doc = valid();
    doc["name"] = 7;
    EXPECT_TRUE(contains(error_of(doc), "name: expected a string")) << error_of(doc);
    EXPECT_FALSE(error_of(json::array()).empty());
}

TEST(GameProject, ValidatesIds) {
    EXPECT_OK(project::validate_game_id("dev.quat3d.coin-collector"));
    EXPECT_OK(project::validate_game_id("com.Example.Game2"));
    for (const char* bad : {"", "game", "com..game", ".com.game", "com.game.", "1com.game",
                            "com.my game", "com.my_game", "com.game.-x", "com/game.x"}) {
        EXPECT_ERR(project::validate_game_id(bad)) << bad;
    }
    json doc = valid();
    doc["id"] = "nodots";
    EXPECT_TRUE(contains(error_of(doc), "id: expected at least two")) << error_of(doc);
}

TEST(GameProject, ValidatesVersions) {
    for (const char* good : {"0.1.0", "1.2.3", "10.0.0-rc.1", "1.0.0-beta-2"}) {
        EXPECT_OK(project::validate_game_version(good)) << good;
    }
    for (const char* bad : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.2.3-", "1.2.3-a..b",
                            "1.2.3+build", "v1.2.3", "1.2.x"}) {
        EXPECT_ERR(project::validate_game_version(bad)) << bad;
    }
    EXPECT_EQ(project::numeric_version("1.2.0-rc.1"), "1.2.0");
    EXPECT_EQ(project::numeric_version("3.4.5"), "3.4.5");
}

TEST(GameProject, PathsStayInsideTheProject) {
    EXPECT_OK(project::validate_project_path("assets/scenes/main.json"));
    for (const char* bad : {"", "/etc/passwd", "../outside.json", "assets/../../x.json",
                            "assets//x.json", "assets/./x.json", "assets\\x.json", "C:/x.json",
                            "assets/x.json/", "a\tb"}) {
        EXPECT_ERR(project::validate_project_path(bad)) << bad;
    }
    json doc = valid();
    doc["start_scene"] = "../elsewhere/scene.json";
    EXPECT_TRUE(contains(error_of(doc), "start_scene:")) << error_of(doc);
    doc = valid();
    doc["start_scene"] = "assets/scenes/main.txt";
    EXPECT_TRUE(contains(error_of(doc), "expected a scene file (.json)")) << error_of(doc);
    doc = valid();
    doc["icon"] = "assets/icon.jpg";
    EXPECT_TRUE(contains(error_of(doc), "icon: expected a .png")) << error_of(doc);
    doc = valid();
    doc["content"] = {"assets/scripts", "assets/scripts"};
    EXPECT_TRUE(contains(error_of(doc), "content[1]: 'assets/scripts' is listed twice"))
        << error_of(doc);
    doc["content"] = {"/abs"};
    EXPECT_TRUE(contains(error_of(doc), "content[0]")) << error_of(doc);
}

TEST(GameProject, NamesAreSafeForFolders) {
    json doc = valid();
    for (const char* bad : {"", "a/b", "a:b", " lead", "trail.", "tab\there"}) {
        doc["name"] = bad;
        EXPECT_FALSE(error_of(doc).empty()) << bad;
    }
    doc["name"] = "Tom & Jerry's Game";
    EXPECT_TRUE(error_of(doc).empty()) << error_of(doc);
}

TEST(GameProject, LoadChecksFilesOnDisk) {
    quat::test::TempProject dir("quat_game_project");
    EXPECT_TRUE(contains(project::load_game_project(dir.path()).error().message, "no game.json"));

    dir.write("game.json", quat::test::game_json("assets/scenes/main.json",
                                                 R"("icon": "assets/icon.png", "content": ["assets/scripts"])"));
    auto e = project::load_game_project(dir.path());
    ASSERT_ERR(e);
    EXPECT_TRUE(contains(e.error().message, "start_scene: no such file 'assets/scenes/main.json'"))
        << e.error().message;

    dir.write("assets/scenes/main.json", R"({"entities": []})");
    e = project::load_game_project(dir.path());
    ASSERT_ERR(e);
    EXPECT_TRUE(contains(e.error().message, "icon: no such file")) << e.error().message;

    dir.write("assets/icon.png", "png");
    e = project::load_game_project(dir.path());
    ASSERT_ERR(e);
    EXPECT_TRUE(contains(e.error().message, "content[0]: no such file or directory"))
        << e.error().message;

    dir.write("assets/scripts/a.lua", "return {}");
    auto ok = project::load_game_project(dir.path());
    ASSERT_OK(ok);
    EXPECT_EQ(ok->name, "Test Game");

    dir.write("game.json", "{ not json");
    EXPECT_TRUE(contains(project::load_game_project(dir.path()).error().message, "not valid JSON"));
}

TEST(GameProject, SymlinkedPathsMayNotLeaveTheProject) {
    quat::test::TempProject outside("quat_game_outside");
    quat::test::TempProject dir("quat_game_project");
    outside.write("scene.json", R"({"entities": []})");
    std::error_code ec;
    std::filesystem::create_directories(dir.path() / "assets", ec);
    std::filesystem::create_directory_symlink(outside.path(), dir.path() / "assets" / "linked", ec);
    if (ec) GTEST_SKIP() << "no symlinks here: " << ec.message();
    dir.write("game.json", quat::test::game_json("assets/linked/scene.json"));
    auto e = project::load_game_project(dir.path());
    ASSERT_ERR(e);
    EXPECT_TRUE(contains(e.error().message, "escapes the project root")) << e.error().message;
}

TEST(GameProject, JsonRoundTrip) {
    auto p = project::parse_game_project(valid());
    ASSERT_OK(p);
    auto again = project::parse_game_project(project::game_project_json(*p));
    ASSERT_OK(again);
    EXPECT_EQ(project::game_project_json(*again), project::game_project_json(*p));
}

TEST(GameProject, OutputFolderMayNotOverlapTheGame) {
    auto p = project::parse_game_project(valid()); // content: assets/scripts, assets/audio/music.ogg
    ASSERT_OK(p);
    const std::filesystem::path root = std::filesystem::absolute("game_root_for_test");
    const auto err = [&](const std::filesystem::path& out) {
        auto r = project::check_output_folder(*p, root, out);
        return r ? std::string() : r.error().message;
    };
    EXPECT_EQ(err("dist"), "");
    EXPECT_EQ(err("build/packages"), "");
    EXPECT_EQ(err(root / "dist"), "");
    EXPECT_EQ(err(root.parent_path() / "elsewhere"), ""); // outside the project
    EXPECT_EQ(err("assets/dist"), ""); // assets/ itself is not shipped whole here
    EXPECT_EQ(err("assets/scripts/dist"),
              "output: 'assets/scripts/dist' is inside the content root 'assets/scripts'");
    EXPECT_EQ(err("assets/scripts"), "output: 'assets/scripts' is the content root 'assets/scripts'");
    EXPECT_EQ(err("assets/audio"), "output: 'assets/audio' contains the content root 'assets/audio/music.ogg'");
    EXPECT_EQ(err(root / "assets" / "scenes" / "out"),
              "output: 'assets/scenes/out' is inside the start scene's folder 'assets/scenes'");
    EXPECT_EQ(err("assets/scenes/"), "output: 'assets/scenes' is the start scene's folder 'assets/scenes'");
    EXPECT_EQ(err("assets"), "output: 'assets' contains the content root 'assets/scripts'");
    EXPECT_EQ(err("."), "output: the project root itself holds the game's files");
    EXPECT_EQ(err("x/../assets/scripts/y"),
              "output: 'assets/scripts/y' is inside the content root 'assets/scripts'");

    p->content = {"assets"};
    EXPECT_EQ(err("assets/dist"), "output: 'assets/dist' is inside the content root 'assets'");
}

} // namespace
