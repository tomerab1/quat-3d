// stage_game_data + the quat-pack driver (ROADMAP P4): the staged data root,
// its manifest, scene rewriting, cooking through quat-cook, determinism, and
// the command line.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/core/utf8_path.hpp"
#include "engine/pack/pack_cli.hpp"
#include "engine/pack/digest.hpp"
#include "engine/pack/stage.hpp"
#include "engine/project/build_job.hpp"
#include "engine/project/game_project.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_project.hpp"

namespace {

namespace fs = std::filesystem;
namespace project = engine::project;
using nlohmann::json;

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

class Stage : public ::testing::Test {
protected:
    quat::test::TempProject game{"quat_stage_game"};
    quat::test::TempProject out{"quat_stage_out"};

    void SetUp() override {
        game.write("game.json", quat::test::game_json());
        game.write("assets/scenes/main.json", R"({"entities": [
            {"name": "Crate", "mesh": {"gltf": "assets/meshes/crate.gltf"}},
            {"name": "Game", "script": {"path": "main.lua"}}]})");
        game.write("assets/meshes/crate.gltf", R"({"buffers": [{"uri": "crate.bin"}]})");
        game.write("assets/meshes/crate.bin", "0123");
        game.write("assets/scripts/main.lua", "print('hi')");
        game.write("assets/unused.txt", "not shipped");
        out.write("shaders/mesh.spv", "spv");
        out.write("shaders/mesh.json", "{}");
        out.write("shaders/lib.slang-module", "not shipped");
    }

    engine::pack::StageOptions options(const std::string& data) {
        engine::pack::StageOptions o;
        o.project_root = game.path();
        auto p = project::load_game_project(game.path());
        EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message);
        if (p) o.project = *p;
        auto files = engine::pack::collect_game_files(game.path(), o.project);
        EXPECT_TRUE(files.has_value()) << (files ? "" : files.error().message);
        if (files) o.files = *files;
        o.data_dir = out.path() / data;
        o.shader_dir = out.path() / "shaders";
        o.target = "linux-x64";
        o.engine_version = "quat-3d test";
        return o;
    }
};

TEST_F(Stage, WritesTheDataRootAndManifest) {
    auto m = engine::pack::stage_game_data(options("data"));
    ASSERT_OK(m);
    for (const char* f : {"data/game.json", "data/manifest.json", "data/assets/scenes/main.json",
                          "data/assets/meshes/crate.gltf", "data/assets/meshes/crate.bin",
                          "data/assets/scripts/main.lua", "data/cooked/shaders/mesh.spv",
                          "data/cooked/shaders/mesh.json"}) {
        EXPECT_TRUE(out.exists(f)) << f;
    }
    EXPECT_FALSE(out.exists("data/assets/unused.txt"));
    EXPECT_FALSE(out.exists("data/cooked/shaders/lib.slang-module"));

    const json doc = json::parse(out.read("data/manifest.json"));
    auto parsed = project::parse_manifest(doc);
    ASSERT_OK(parsed);
    EXPECT_EQ(parsed->game_id, "dev.quat3d.test-game");
    EXPECT_EQ(parsed->config, project::PackageConfig::shipping);
    ASSERT_EQ(parsed->files.size(), 7U);
    EXPECT_TRUE(std::is_sorted(parsed->files.begin(), parsed->files.end(),
                               [](const auto& a, const auto& b) { return a.path < b.path; }));
    for (const project::ManifestFile& f : parsed->files) {
        std::uint64_t bytes = 0;
        auto sha = engine::pack::sha256_file(out.path() / "data" / f.path, bytes);
        ASSERT_OK(sha);
        EXPECT_EQ(f.sha256, *sha) << f.path;
        EXPECT_EQ(f.bytes, bytes) << f.path;
        EXPECT_FALSE(f.cooked);
    }
    const auto shader = std::find_if(parsed->files.begin(), parsed->files.end(),
                                     [](const auto& f) { return f.path == "cooked/shaders/mesh.spv"; });
    ASSERT_NE(shader, parsed->files.end());
    EXPECT_EQ(shader->kind, project::FileKind::shader);

    // The staging directory must be fresh.
    EXPECT_ERR(engine::pack::stage_game_data(options("data")));
}

TEST_F(Stage, IsDeterministic) {
    ASSERT_OK(engine::pack::stage_game_data(options("a")));
    ASSERT_OK(engine::pack::stage_game_data(options("b")));
    EXPECT_EQ(out.read("a/manifest.json"), out.read("b/manifest.json"));
}

TEST_F(Stage, RewritesAbsoluteScenePaths) {
    const std::string abs =
        engine::core::path_to_generic_utf8(fs::absolute(game.path() / "assets/meshes/crate.gltf"));
    json scene = json::parse(R"({"entities": [{"name": "Crate", "mesh": {}}]})");
    scene["entities"][0]["mesh"] = {{"gltf", abs}, {"mesh_key", abs + "#mesh0.prim0"}};
    game.write("assets/scenes/main.json", scene.dump());
    ASSERT_OK(engine::pack::stage_game_data(options("data")));
    const json staged = json::parse(out.read("data/assets/scenes/main.json"));
    EXPECT_EQ(staged["entities"][0]["mesh"]["gltf"], "assets/meshes/crate.gltf");
    EXPECT_EQ(staged["entities"][0]["mesh"]["mesh_key"], "assets/meshes/crate.gltf#mesh0.prim0");
    EXPECT_EQ(game.read("assets/scenes/main.json"), scene.dump()); // the source is untouched
}

TEST_F(Stage, NeedsShaders) {
    auto o = options("data");
    o.shader_dir = out.path() / "nowhere";
    auto m = engine::pack::stage_game_data(o);
    ASSERT_ERR(m);
    EXPECT_TRUE(contains(m.error().message, "no cooked shaders")) << m.error().message;
}

#if !defined(_WIN32)
// A stand-in quat-cook: writes a .gltf whose data lives in a new sidecar.
TEST_F(Stage, CooksModelsWithQuatCook) {
    out.write("quat-cook", "#!/bin/sh\n"
                           "printf '{\"buffers\": [{\"uri\": \"crate.cooked.bin\"}]}' > \"$2\"\n"
                           "printf 'cooked' > \"$(dirname \"$2\")/crate.cooked.bin\"\n");
    fs::permissions(out.path() / "quat-cook", fs::perms::owner_all);
    auto o = options("data");
    o.quat_cook = out.path() / "quat-cook";
    auto m = engine::pack::stage_game_data(o);
    ASSERT_OK(m);
    EXPECT_TRUE(out.exists("data/assets/meshes/crate.cooked.bin"));
    EXPECT_FALSE(out.exists("data/assets/meshes/crate.bin")); // superseded by the cooked data
    std::vector<std::string> cooked;
    for (const auto& f : m->files) {
        if (f.cooked) cooked.push_back(f.path);
    }
    EXPECT_EQ(cooked, (std::vector<std::string>{"assets/meshes/crate.cooked.bin",
                                                "assets/meshes/crate.gltf"}));

    out.write("quat-cook", "#!/bin/sh\necho 'bad texture' >&2\nexit 3\n");
    o.data_dir = out.path() / "data2";
    auto failed = engine::pack::stage_game_data(o);
    ASSERT_ERR(failed);
    EXPECT_TRUE(contains(failed.error().message, "quat-cook failed on 'assets/meshes/crate.gltf'"));
    EXPECT_TRUE(contains(failed.error().message, "bad texture"));
}
#endif

TEST(PackCli, ParsesArguments) {
    using engine::pack::parse_pack_args;
    const std::vector<std::string> args{"games/demo", "--player", "bin/quat-player", "--out",
                                        "dist", "--config", "development", "--build-id",
                                        "dev.abc1234", "--cook", "off", "--smoke-test"};
    auto o = parse_pack_args(args);
    ASSERT_OK(o);
    EXPECT_EQ(o->project, fs::path("games/demo"));
    EXPECT_EQ(o->player, fs::path("bin/quat-player"));
    EXPECT_EQ(o->config, project::PackageConfig::development);
    EXPECT_EQ(o->build_id, "dev.abc1234");
    EXPECT_EQ(o->cook, engine::pack::CookMode::off);
    EXPECT_TRUE(o->smoke_test);

    const auto err = [&](std::vector<std::string> a) {
        auto r = parse_pack_args(a);
        return r ? std::string() : r.error().message;
    };
    EXPECT_TRUE(contains(err({"--out", "d", "--player", "p"}), "no game project"));
    EXPECT_TRUE(contains(err({"g", "--player", "p"}), "--out is required"));
    EXPECT_TRUE(contains(err({"g", "--out", "d"}), "--player is required"));
    EXPECT_TRUE(contains(err({"g", "--out", "d", "--player"}), "--player: missing value"));
    EXPECT_TRUE(contains(err({"g", "h", "--out", "d", "--player", "p"}), "more than one project"));
    EXPECT_TRUE(contains(err({"g", "--out", "d", "--player", "p", "--frobnicate"}), "unknown option"));
    EXPECT_TRUE(contains(err({"g", "--out", "d", "--player", "p", "--config", "debug"}), "--config"));
    EXPECT_TRUE(contains(err({"g", "--out", "d", "--player", "p", "--build-id", "a b"}), "--build-id"));
    EXPECT_TRUE(contains(err({"g", "--out", "d", "--stage-only", "--smoke-test"}), "--smoke-test"));
    EXPECT_OK(parse_pack_args(std::vector<std::string>{"g", "--out", "d", "--stage-only"}));
}

TEST(PackCli, BuildIds) {
    EXPECT_OK(engine::pack::validate_build_id("dev.abc1234"));
    EXPECT_OK(engine::pack::validate_build_id("42"));
    EXPECT_ERR(engine::pack::validate_build_id(""));
    EXPECT_ERR(engine::pack::validate_build_id("-x"));
    EXPECT_ERR(engine::pack::validate_build_id("a/b"));
    EXPECT_ERR(engine::pack::validate_build_id(std::string(65, 'a')));
}

TEST_F(Stage, StageOnlyRunReportsStepsAndFolder) {
    engine::pack::PackOptions o;
    o.project = game.path() / "game.json";
    o.out = out.path() / "dist";
    o.stage_only = true;
    o.shaders = out.path() / "shaders";
    o.cook = engine::pack::CookMode::off;
    std::string log;
    const int code = engine::pack::run_pack(o, {}, [&](std::string_view s) { log += s; });
    EXPECT_EQ(code, 0) << log;
    EXPECT_TRUE(contains(log, "[pack] step 1/3 validate game project")) << log;
    EXPECT_TRUE(contains(log, "[pack] step 2/3 collect game files")) << log;
    EXPECT_TRUE(contains(log, "[pack] step 3/3 stage data")) << log;
    EXPECT_TRUE(contains(log, "[pack] folder ")) << log;
    EXPECT_TRUE(out.exists("dist/.quat-pack/test-game-" + std::string(project::host_target()) +
                           "/data/manifest.json"))
        << log;

    // Errors end the run with a machine-readable line.
    game.write("game.json", "{}");
    log.clear();
    EXPECT_EQ(engine::pack::run_pack(o, {}, [&](std::string_view s) { log += s; }), 1);
    EXPECT_TRUE(contains(log, "[pack] error: game.json: format: required")) << log;
}

TEST_F(Stage, OutputMayNotOverlapTheGame) {
    engine::pack::PackOptions o;
    o.project = game.path();
    o.out = game.path() / "assets" / "scenes" / "dist";
    o.stage_only = true;
    o.shaders = out.path() / "shaders";
    std::string log;
    EXPECT_EQ(engine::pack::run_pack(o, {}, [&](std::string_view s) { log += s; }), 1);
    EXPECT_TRUE(contains(log, "[pack] error: --out: 'assets/scenes/dist' is inside the start "
                              "scene's folder 'assets/scenes'"))
        << log;
    EXPECT_FALSE(game.exists("assets/scenes/dist"));
}

TEST(PackCli, ShippingPackagesUseTheShippingPlayer) {
    quat::test::TempProject bin("quat_pack_players");
#if defined(_WIN32)
    const std::string ext = ".exe";
#else
    const std::string ext;
#endif
    bin.write("quat-player" + ext, "dev");
    const fs::path dev = bin.path() / ("quat-player" + ext);
    const fs::path ship = bin.path() / ("quat-player-shipping" + ext);
    using engine::pack::player_for_config;

    auto missing = player_for_config(dev, project::PackageConfig::shipping);
    ASSERT_ERR(missing);
    EXPECT_TRUE(contains(missing.error().message, "build the quat-player-shipping target"));

    bin.write("quat-player-shipping" + ext, "ship");
    auto shipping = player_for_config(dev, project::PackageConfig::shipping);
    ASSERT_OK(shipping);
    EXPECT_EQ(*shipping, ship);
    EXPECT_EQ(*player_for_config(ship, project::PackageConfig::shipping), ship);
    EXPECT_EQ(*player_for_config(dev, project::PackageConfig::development), dev);
    auto wrong = player_for_config(ship, project::PackageConfig::development);
    ASSERT_ERR(wrong);
    EXPECT_TRUE(contains(wrong.error().message, "no remote control"));
}

} // namespace
