// Game modules (ROADMAP G17.1): registration through engine::game::Registrar,
// every problem collected with the module's name, systems ordered within
// their phase, methods validated against their JSON Schemas.

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/game/module_set.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

using nlohmann::json;
namespace game = engine::game;
namespace scene = engine::scene;

std::vector<std::string> g_ran; // systems record their runs (test-only state)

void sys_a(game::SystemContext&, float) { g_ran.emplace_back("a"); }
void sys_b(game::SystemContext&, float) { g_ran.emplace_back("b"); }
void sys_c(game::SystemContext&, float) { g_ran.emplace_back("c"); }

struct Marker {
    float level = 1.0F;
};

constexpr std::string_view k_after_a[] = {"test.a"};
constexpr std::string_view k_after_b[] = {"test.b"};
constexpr std::string_view k_after_c[] = {"test.c"};
constexpr std::string_view k_after_x[] = {"test.missing"};

std::expected<void, engine::core::Error> good_module(game::Registrar& r) {
    r.component<Marker>("test_marker", "Test marker",
                        game::ComponentFields<Marker>().field("level", &Marker::level, {.min = 0, .max = 5}));
    // Registered out of order; `after` decides.
    r.system({.name = "test.c", .phase = game::Phase::post_physics, .run = &sys_c, .after = k_after_b});
    r.system({.name = "test.b", .phase = game::Phase::post_physics, .run = &sys_b, .after = k_after_a});
    r.system({.name = "test.a", .phase = game::Phase::post_physics, .run = &sys_a});
    r.method({.name = "test.echo",
              .summary = "Echo",
              .params_schema = json::parse(R"({"type": "object", "required": ["n"],
                  "properties": {"n": {"type": "integer", "minimum": 0}}, "additionalProperties": false})"),
              .handler = [](game::SystemContext& ctx, const json& p) -> std::expected<json, engine::core::Error> {
                  return json{{"n", p["n"]}, {"entities", ctx.registry().storage<entt::entity>().size()}};
              }});
    return {};
}

std::expected<void, engine::core::Error> bad_module(game::Registrar& r) {
    r.component<Marker>("test_marker2", "x", game::ComponentFields<Marker>().field("level", &Marker::level));
    r.system({.name = "Bad Name", .phase = game::Phase::input, .run = &sys_a});
    r.system({.name = "test.lonely", .phase = game::Phase::input, .run = &sys_a, .after = k_after_x});
    r.method({.name = "scene.mine", .summary = "", .handler = [](game::SystemContext&, const json&)
                  -> std::expected<json, engine::core::Error> { return json(); }});
    r.method({.name = "test.schema",
              .params_schema = json::parse(R"({"type": "object", "pattern": "x"})"),
              .handler = [](game::SystemContext&, const json&) -> std::expected<json, engine::core::Error> {
                  return json();
              }});
    r.ui_screen({.name = "hud", .layout_path = "ui/hud.json"});
    r.save_chunk({.name = "town", .version = 1});
    r.input_actions("../outside.json");
    return std::unexpected(engine::core::Error{"the module's own check failed"});
}

TEST(ModuleSet, LoadsAModuleAndOrdersItsSystems) {
    scene::ComponentRegistry components;
    const game::ModuleEntry modules[] = {{"test_game", &good_module}};
    auto set = game::ModuleSet::load(modules, components, {});
    ASSERT_OK(set);
    EXPECT_NE(components.find("test_marker"), nullptr);

    scene::Scene world;
    game::Services services;
    game::SystemContext ctx(world, services);
    g_ran.clear();
    set->run(game::Phase::post_physics, ctx, 0.1F);
    EXPECT_EQ(g_ran, (std::vector<std::string>{"a", "b", "c"}));
    g_ran.clear();
    set->run(game::Phase::input, ctx, 0.1F);
    EXPECT_TRUE(g_ran.empty());

    auto echo = set->call("test.echo", ctx, {{"n", 3}});
    ASSERT_OK(echo);
    EXPECT_EQ((*echo)["n"], 3);
    auto bad = set->call("test.echo", ctx, {{"n", -1}});
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error().message.find("params.n: expected a number >= 0"), std::string::npos)
        << bad.error().message;
    EXPECT_FALSE(set->call("test.echo", ctx, json::object())); // n is required
    EXPECT_FALSE(set->call("test.nope", ctx, json::object()));
}

TEST(ModuleSet, ReportsEveryProblemAndRegistersNothing) {
    scene::ComponentRegistry components;
    const game::ModuleEntry modules[] = {{"broken", &bad_module}};
    auto set = game::ModuleSet::load(modules, components, {});
    ASSERT_FALSE(set);
    const std::string& e = set.error().message;
    for (const char* part : {"module 'broken': the module's own check failed",
                             "system 'Bad Name'",
                             "'test.missing', which is not a system of the input phase",
                             "the namespace 'scene' belongs to the engine",
                             "params_schema pattern: unsupported keyword",
                             "UI screens need ROADMAP G6.1",
                             "save chunks need ROADMAP G11.1",
                             "input actions '../outside.json'"}) {
        EXPECT_NE(e.find(part), std::string::npos) << part << "\n" << e;
    }
    EXPECT_EQ(components.find("test_marker2"), nullptr) << "a failed load unregisters its components";
}

std::expected<void, engine::core::Error> cyclic(game::Registrar& r) {
    r.system({.name = "test.a", .phase = game::Phase::pre_render, .run = &sys_a, .after = k_after_c});
    r.system({.name = "test.c", .phase = game::Phase::pre_render, .run = &sys_c, .after = k_after_a});
    return {};
}

TEST(ModuleSet, RefusesCyclesDuplicatesAndMissingModules) {
    scene::ComponentRegistry components;
    const game::ModuleEntry cycle[] = {{"cyclic", &cyclic}};
    auto c = game::ModuleSet::load(cycle, components, {});
    ASSERT_FALSE(c);
    EXPECT_NE(c.error().message.find("cycle"), std::string::npos) << c.error().message;

    const game::ModuleEntry twice[] = {{"m", &good_module}, {"m", &good_module}};
    auto t = game::ModuleSet::load(twice, components, {});
    ASSERT_FALSE(t);
    EXPECT_NE(t.error().message.find("listed twice"), std::string::npos);
    EXPECT_EQ(components.size(), 0U);

    const game::ModuleEntry none[] = {{"empty", nullptr}};
    EXPECT_FALSE(game::ModuleSet::load(none, components, {}));
}

TEST(ModuleSet, ModuleActionMapsMergeStrictly) {
    engine::input::ActionMap base;
    engine::input::ActionDesc jump;
    jump.name = "jump";
    base.actions.push_back(jump);
    engine::input::ActionMap extra;
    engine::input::ActionDesc build;
    build.name = "build";
    extra.actions.push_back(build);
    EXPECT_OK(game::add_module_action_map(base, extra));
    EXPECT_EQ(base.actions.size(), 2U);
    auto again = game::add_module_action_map(base, extra);
    ASSERT_FALSE(again);
    EXPECT_NE(again.error().message.find("'build' is defined twice"), std::string::npos);
}

} // namespace
