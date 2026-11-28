// Remote command modules (ROADMAP S9): the world commands are registered from
// a table the build generates from src/remote/world/*_command*.cpp, and the
// scene methods dispatch through the component registry. A new command module
// or a new component edits no central list.

#include <filesystem>
#include <set>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "engine/core/json_fields.hpp"
#include "engine/scene/component_registry.hpp"
#include "remote/world/internal.hpp"
#include "world_fixture.hpp"

namespace {

using quat::test::WorldCommands;
using engine::remote::Json;

TEST(CommandModules, GeneratedFromTheWorldDirectory) {
    std::set<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::path(QUAT_PROJECT_ROOT) / "src" / "remote" / "world")) {
        const std::string stem = entry.path().stem().string();
        if (entry.path().extension() == ".cpp" && stem.find("_command") != std::string::npos) {
            files.insert(stem);
        }
    }
    std::set<std::string> generated;
    for (const std::string_view n : engine::remote::world::command_module_sources()) {
        generated.emplace(n);
    }
    EXPECT_FALSE(files.empty());
    EXPECT_EQ(files, generated);
}

// One method of each module answers, through the generated registration.
TEST_F(WorldCommands, EveryModuleRegistersItsMethods) {
    register_commands();
    std::set<std::string> methods;
    for (const auto& m : server.methods()) methods.insert(m.name);
    for (const char* name :
         {"scene.list", "scene.batch", "environment.get", "terrain.status", "world.place_on_ground",
          "camera.get", "debug.step", "render.settings.get", "bt.get", "physics.get", "weather.get",
          "project.info", "script.list", "profile.frame", "shader.list", "time.get", "input.actions",
          "camera.rig.get"}) {
        EXPECT_TRUE(methods.contains(name)) << name;
    }
}

// A component registered at run time is listed, created, read and removed by
// the scene methods with no change to remote code.
TEST_F(WorldCommands, ANewComponentReachesTheSceneMethods) {
    struct Marker {
        int level = 1;
    };
    engine::scene::ComponentCodec codec{.name = "test_marker", .summary = "Test marker"};
    codec.encode = [](const entt::registry& r, entt::entity e, const engine::scene::EncodeOptions&) {
        return Json{{"level", r.get<Marker>(e).level}};
    };
    codec.apply = [](entt::registry& r, entt::entity e, const Json& v, bool strict)
        -> std::expected<void, engine::core::Error> {
        Marker m = r.all_of<Marker>(e) ? r.get<Marker>(e) : Marker{};
        engine::core::JsonFields f(v, "test_marker", strict);
        f.integer("level", m.level, 0, 9);
        if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
        r.emplace_or_replace<Marker>(e, m);
        return {};
    };
    auto& registry = engine::scene::component_registry();
    ASSERT_TRUE(registry.add(engine::scene::typed_codec<Marker>(std::move(codec))));
    register_commands();

    bool listed = false;
    for (const Json& c : ok("scene.components")) listed = listed || c["name"] == "test_marker";
    EXPECT_TRUE(listed);
    const Json created = ok("scene.create", {{"name", "m"}, {"components", {{"test_marker", {{"level", 4}}}}}});
    const Json got = ok("scene.get", {{"entity", created["id"]}});
    EXPECT_EQ(got["components"]["test_marker"]["level"], 4);
    const std::string message = err("scene.update",
                                    {{"entity", created["id"]},
                                     {"components", {{"test_marker", {{"level", 99}}}}}},
                                    engine::remote::rpc_code::invalid_params);
    EXPECT_NE(message.find("test_marker.level"), std::string::npos) << message;
    (void)ok("scene.remove_components", {{"entity", created["id"]}, {"components", {"test_marker"}}});
    EXPECT_FALSE(reg().all_of<Marker>(entity(created["id"])));
    EXPECT_TRUE(registry.remove("test_marker"));
}

} // namespace
