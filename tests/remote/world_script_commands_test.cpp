// G7: script.* remote commands — listing, reading, creating and updating
// scripts with compile checks (syntax errors carry the line), path
// confinement to assets/scripts/, and hot reload of a running session.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "engine/script/script_runtime.hpp"
#include "remote/world_fixture.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using quat::test::Json;
namespace code = engine::remote::rpc_code;

class WorldScripts : public quat::test::WorldCommands {
protected:
    quat::test::TempDir root{"script-cmds"};
    std::optional<engine::script::ScriptRuntime> runtime;

    void SetUp() override {
        fs::create_directories(root.path() / "assets" / "scripts");
        runtime.emplace(engine::script::ScriptRuntime::Options{
            root.path() / "assets" / "scripts", {}, false});
        services.project_root = [this] { return root.path(); };
        services.scripts = [this] { return &*runtime; };
        register_commands();
    }
    [[nodiscard]] std::string read(const std::string& rel) const {
        std::ifstream in(root.path() / "assets" / "scripts" / rel);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
};

TEST_F(WorldScripts, CreateGetListUpdateDelete) {
    const Json created = ok("script.create", {{"path", "game/coin.lua"},
                                              {"source", "function on_start(self) end"}});
    EXPECT_EQ(created["path"], "game/coin.lua");
    EXPECT_EQ(read("game/coin.lua"), "function on_start(self) end");
    // Creating it twice is refused; update needs an existing file.
    EXPECT_NE(err("script.create", {{"path", "game/coin.lua"}, {"source", "x = 1"}}, code::failed)
                  .find("already exists"),
              std::string::npos);
    EXPECT_NE(err("script.update", {{"path", "nope.lua"}, {"source", "x = 1"}}, code::not_found)
                  .find("script.create"),
              std::string::npos);

    const Json got = ok("script.get", {{"path", "game/coin.lua"}});
    EXPECT_EQ(got["source"], "function on_start(self) end");

    ok("script.update", {{"path", "game/coin.lua"}, {"source", "x = 2"}});
    EXPECT_EQ(read("game/coin.lua"), "x = 2");

    const Json list = ok("script.list");
    ASSERT_EQ(list["scripts"].size(), 1U);
    EXPECT_EQ(list["scripts"][0]["path"], "game/coin.lua");
    EXPECT_EQ(list["running"], false);

    const Json deleted = ok("script.delete", {{"path", "game/coin.lua"}});
    EXPECT_EQ(deleted["deleted"], true);
    EXPECT_FALSE(fs::exists(root.path() / "assets" / "scripts" / "game" / "coin.lua"));
    err("script.get", {{"path", "game/coin.lua"}}, code::not_found);
}

TEST_F(WorldScripts, SyntaxErrorsCarryTheLineAndWriteNothing) {
    auto r = call("script.create",
                  {{"path", "bad.lua"}, {"source", "function on_start(self)\n  local = 1\nend"}});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, code::invalid_params);
    EXPECT_NE(r.error().message.find("syntax error at line 2"), std::string::npos) << r.error().message;
    EXPECT_NE(r.error().message.find("bad.lua:2:"), std::string::npos);
    EXPECT_EQ(r.error().data["line"], 2);
    EXPECT_FALSE(fs::exists(root.path() / "assets" / "scripts" / "bad.lua"));

    const Json check = ok("script.check", {{"source", "x = = 1"}});
    EXPECT_EQ(check["ok"], false);
    EXPECT_EQ(check["line"], 1);
    EXPECT_EQ(ok("script.check", {{"source", "return 1"}})["ok"], true);
}

TEST_F(WorldScripts, PathsAreConfined) {
    for (const char* bad : {"../escape.lua", "/abs.lua", "x/../../y.lua", "notes.txt", "a b.lua"}) {
        err("script.create", {{"path", bad}, {"source", "x = 1"}}, code::invalid_params);
    }
    // A symlink inside assets/scripts that leads out is refused as well.
    std::error_code ec;
    fs::create_directories(root.path() / "outside", ec);
    fs::create_directory_symlink(root.path() / "outside", root.path() / "assets" / "scripts" / "link", ec);
    if (!ec) {
        EXPECT_NE(err("script.create", {{"path", "link/evil.lua"}, {"source", "x = 1"}},
                      code::invalid_params)
                      .find("outside"),
                  std::string::npos);
        EXPECT_FALSE(fs::exists(root.path() / "outside" / "evil.lua"));
    }
    err("script.create", {{"path", "big.lua"}, {"source", std::string(300 * 1024, 'x')}},
        code::invalid_params);
    err("script.create", {{"path", "x.lua"}}, code::invalid_params); // no source
    err("script.create", {{"path", "x.lua"}, {"source", "x = 1"}, {"sauce", 1}}, code::invalid_params);
}

TEST_F(WorldScripts, UpdateHotReloadsARunningSession) {
    ok("script.create", {{"path", "hello.lua"},
                         {"source", "function on_update(self, dt) self.entity:set_name('v1') end"}});
    const entt::entity e = scene.create_entity("thing");
    ok("scene.update", {{"entity", static_cast<std::uint32_t>(e)},
                        {"components", {{"script", {{"path", "hello.lua"}}}}}});
    ASSERT_TRUE(runtime->start(scene).has_value());
    runtime->update(1.0F / 60.0F);
    EXPECT_EQ(reg().get<engine::scene::Name>(e).value, "v1");

    const Json updated = ok("script.update", {{"path", "hello.lua"},
                                              {"source", "function on_update(self, dt) self.entity:set_name('v2') end"}});
    EXPECT_EQ(updated["running"], true);
    EXPECT_EQ(updated["status"]["state"], "ok");
    EXPECT_EQ(updated["status"]["reloads"], 1);
    runtime->update(1.0F / 60.0F);
    EXPECT_EQ(reg().get<engine::scene::Name>(e).value, "v2");

    const Json list = ok("script.list");
    EXPECT_EQ(list["scripts"][0]["entities"], 1);
    EXPECT_EQ(list["scripts"][0]["status"]["instances"], 1);
}

TEST_F(WorldScripts, ScriptComponentErrorsAreRemoteErrors) {
    const entt::entity e = scene.create_entity("thing");
    const std::string msg =
        err("scene.update",
            {{"entity", static_cast<std::uint32_t>(e)},
             {"components", {{"script", {{"path", "../x.lua"}}}}}},
            code::invalid_params);
    EXPECT_NE(msg.find("script.path"), std::string::npos) << msg;
}

} // namespace
