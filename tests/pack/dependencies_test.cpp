// collect_game_files (ROADMAP P4): only what the game references ships —
// followed from the start scene through models, audio and scripts, plus
// content roots; every path confined to the project.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/core/utf8_path.hpp"
#include "engine/pack/dependencies.hpp"
#include "engine/project/game_project.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_project.hpp"

namespace {

namespace fs = std::filesystem;
namespace project = engine::project;
using engine::pack::GameFiles;
using project::FileKind;

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

const engine::pack::PackFile* find(const GameFiles& f, const std::string& path) {
    const auto it = std::find_if(f.files.begin(), f.files.end(),
                                 [&](const auto& p) { return p.path == path; });
    return it == f.files.end() ? nullptr : &*it;
}

std::vector<std::string> paths(const GameFiles& f) {
    std::vector<std::string> out;
    for (const auto& p : f.files) out.push_back(p.path);
    return out;
}

// A small game: a scene with a glTF (+ buffer + texture), an audio clip and a
// script that requires a module and spawns another scripted entity.
class Dependencies : public ::testing::Test {
protected:
    quat::test::TempProject dir{"quat_pack_deps"};

    void SetUp() override {
        dir.write("game.json", quat::test::game_json("assets/scenes/main.json",
                                                     R"("icon": "assets/icon.png", "content": ["assets/extra"])"));
        dir.write("assets/icon.png", "png");
        dir.write("assets/scenes/main.json", R"({"entities": [
            {"name": "Crate", "mesh": {"mesh_key": "assets/meshes/crate.gltf#mesh0.prim0",
                                       "gltf": "assets/meshes/crate.gltf"}},
            {"name": "Music", "audio_source": {"clip": "assets/audio/theme.ogg"}},
            {"name": "Game", "script": {"path": "game/main.lua"}}
        ]})");
        dir.write("assets/meshes/crate.gltf",
                  R"({"buffers": [{"uri": "crate.bin"}], "images": [{"uri": "tex/crate%20albedo.png"}]})");
        dir.write("assets/meshes/crate.bin", "bin");
        dir.write("assets/meshes/tex/crate albedo.png", "img");
        dir.write("assets/audio/theme.ogg", "ogg");
        dir.write("assets/scripts/game/main.lua",
                  "local util = require(\"lib.util\")\n"
                  "-- \"commented/out.lua\"\n"
                  "scene.spawn({script = {path = \"game/coin.lua\"}})\n"
                  "scene.spawn_model(\"assets/meshes/crate.gltf\")\n"
                  "print(\"Score: 1\")\n"
                  "local maybe = \"game/missing.lua\"\n");
        dir.write("assets/scripts/lib/util.lua", "return {}");
        dir.write("assets/scripts/game/coin.lua", "return {}");
        dir.write("assets/scripts/unused.lua", "return {}");
        dir.write("assets/meshes/unused.glb", "glb");
        dir.write("assets/extra/level2.json", R"({"entities": [
            {"name": "Sfx", "audio_source": {"clip": "assets/audio/hit.wav"}}]})");
        dir.write("assets/extra/notes.json", R"({"not": "a scene"})");
        dir.write("assets/extra/.hidden", "x");
        dir.write("assets/extra/.git/config", "x");
        dir.write("assets/audio/hit.wav", "wav");
    }

    std::expected<GameFiles, engine::core::Error> collect() {
        auto p = project::load_game_project(dir.path());
        if (!p) return std::unexpected(p.error());
        return engine::pack::collect_game_files(dir.path(), *p);
    }
};

TEST_F(Dependencies, FollowsReferences) {
    auto files = collect();
    ASSERT_OK(files);
    const std::vector<std::string> want{
        "assets/audio/hit.wav",
        "assets/audio/theme.ogg",
        "assets/extra/level2.json",
        "assets/extra/notes.json",
        "assets/icon.png",
        "assets/meshes/crate.bin",
        "assets/meshes/crate.gltf",
        "assets/meshes/tex/crate albedo.png",
        "assets/scenes/main.json",
        "assets/scripts/game/coin.lua",
        "assets/scripts/game/main.lua",
        "assets/scripts/lib/util.lua",
        "game.json",
    };
    EXPECT_EQ(paths(*files), want);
    EXPECT_EQ(find(*files, "game.json")->kind, FileKind::project);
    EXPECT_EQ(find(*files, "assets/icon.png")->kind, FileKind::icon);
    EXPECT_EQ(find(*files, "assets/scenes/main.json")->kind, FileKind::scene);
    EXPECT_EQ(find(*files, "assets/extra/level2.json")->kind, FileKind::scene);
    EXPECT_EQ(find(*files, "assets/extra/notes.json")->kind, FileKind::content);
    EXPECT_EQ(find(*files, "assets/meshes/crate.gltf")->kind, FileKind::model);
    const auto* bin = find(*files, "assets/meshes/crate.bin");
    EXPECT_EQ(bin->kind, FileKind::model_data);
    EXPECT_EQ(bin->owners, std::vector<std::string>{"assets/meshes/crate.gltf"});
    EXPECT_EQ(find(*files, "assets/audio/theme.ogg")->kind, FileKind::audio);
    EXPECT_EQ(find(*files, "assets/scripts/lib/util.lua")->kind, FileKind::script);
    EXPECT_TRUE(contains(find(*files, "assets/scripts/game/coin.lua")->reason, "main.lua' line 3"))
        << find(*files, "assets/scripts/game/coin.lua")->reason;
    // A script literal that names nothing is a warning, not an error.
    ASSERT_EQ(files->warnings.size(), 1U);
    EXPECT_TRUE(contains(files->warnings[0], "game/missing.lua")) << files->warnings[0];
    EXPECT_TRUE(files->scene_rewrites.empty());
}

TEST_F(Dependencies, MissingFilesNameTheirReferrer) {
    dir.write("assets/scenes/main.json", R"({"entities": [
        {"name": "Ghost", "audio_source": {"clip": "assets/audio/nope.ogg"}}]})");
    auto files = collect();
    ASSERT_ERR(files);
    EXPECT_TRUE(contains(files.error().message,
                         "scene 'assets/scenes/main.json' entities[0] 'Ghost' audio_source.clip: no "
                         "such file 'assets/audio/nope.ogg'"))
        << files.error().message;

    dir.write("assets/scenes/main.json",
              R"({"entities": [{"name": "P", "script": {"path": "../escape.lua"}}]})");
    files = collect();
    ASSERT_ERR(files);
    EXPECT_TRUE(contains(files.error().message, "script.path")) << files.error().message;

    dir.write("assets/scenes/main.json", R"({"entities": [{"mesh": {"gltf": "assets/meshes/crate.gltf"}}]})");
    dir.write("assets/meshes/crate.gltf", R"({"buffers": [{"uri": "../../../../etc/passwd"}]})");
    files = collect();
    ASSERT_ERR(files);
    EXPECT_TRUE(contains(files.error().message, "model 'assets/meshes/crate.gltf'"))
        << files.error().message;
}

TEST_F(Dependencies, AbsolutePathsInsideTheProjectAreRewritten) {
    const std::string abs = engine::core::path_to_generic_utf8(
        fs::absolute(dir.path() / "assets" / "meshes" / "crate.gltf"));
    nlohmann::json scene = nlohmann::json::parse(R"({"entities": [{"name": "Crate", "mesh": {}}]})");
    scene["entities"][0]["mesh"] = {{"gltf", abs}, {"mesh_key", abs + "#mesh0.prim0"},
                                    {"material_key", abs + "#mat0"}};
    dir.write("assets/scenes/main.json", scene.dump());
    auto files = collect();
    ASSERT_OK(files);
    ASSERT_NE(find(*files, "assets/meshes/crate.gltf"), nullptr);
    ASSERT_EQ(files->scene_rewrites.count("assets/scenes/main.json"), 1U);
    const auto& rw = files->scene_rewrites.at("assets/scenes/main.json");
    ASSERT_EQ(rw.size(), 1U);
    EXPECT_EQ(rw[0].to, "assets/meshes/crate.gltf");

    EXPECT_EQ(engine::pack::rewrite_scene_paths(scene, rw), 3U);
    EXPECT_EQ(scene["entities"][0]["mesh"]["gltf"], "assets/meshes/crate.gltf");
    EXPECT_EQ(scene["entities"][0]["mesh"]["mesh_key"], "assets/meshes/crate.gltf#mesh0.prim0");
    EXPECT_EQ(scene["entities"][0]["mesh"]["material_key"], "assets/meshes/crate.gltf#mat0");
}

TEST_F(Dependencies, AbsolutePathsOutsideTheProjectFail) {
    quat::test::TempProject other("quat_pack_other");
    other.write("x.gltf", "{}");
    nlohmann::json scene = nlohmann::json::parse(R"({"entities": [{"name": "X", "mesh": {}}]})");
    scene["entities"][0]["mesh"]["gltf"] =
        engine::core::path_to_generic_utf8(fs::absolute(other.path() / "x.gltf"));
    dir.write("assets/scenes/main.json", scene.dump());
    auto files = collect();
    ASSERT_ERR(files);
    EXPECT_TRUE(contains(files.error().message, "outside the game project")) << files.error().message;
}

TEST_F(Dependencies, ContentRootsMayNotEscapeThroughSymlinks) {
    quat::test::TempProject other("quat_pack_other");
    other.write("secret.txt", "x");
    std::error_code ec;
    fs::create_symlink(other.path() / "secret.txt", dir.path() / "assets" / "extra" / "link.txt", ec);
    if (ec) GTEST_SKIP() << "no symlinks here: " << ec.message();
    auto files = collect();
    ASSERT_ERR(files);
    EXPECT_TRUE(contains(files.error().message, "escapes the project root")) << files.error().message;
}

TEST(PackKinds, ByExtension) {
    EXPECT_EQ(engine::pack::kind_for_path("a/b.GLB"), FileKind::model);
    EXPECT_EQ(engine::pack::kind_for_path("a/b.lua"), FileKind::script);
    EXPECT_EQ(engine::pack::kind_for_path("a/b.flac"), FileKind::audio);
    EXPECT_EQ(engine::pack::kind_for_path("a.dir/b"), FileKind::content);
    EXPECT_EQ(engine::pack::kind_for_path("a/b.png"), FileKind::content);
}

} // namespace
