// Coin Collector's native module (ROADMAP G17.1) through the engine's module
// loader: its component decodes and lists a schema, its system spins coins,
// its remote method answers and its Lua function computes the ring.

#include <cmath>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "coin_module.hpp"
#include "engine/game/module_set.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace {

using nlohmann::json;
namespace game = engine::game;

class CoinModule : public ::testing::Test {
protected:
    void SetUp() override {
        const game::ModuleEntry modules[] = {{"coin_collector", &coin_collector::register_module}};
        auto loaded = game::ModuleSet::load(modules, engine::scene::component_registry(),
                                            {.project_root = COIN_COLLECTOR_ROOT});
        ASSERT_TRUE(loaded) << loaded.error().message;
        set = std::move(*loaded);
    }
    void TearDown() override {
        for (const std::string& c : set.components()) engine::scene::component_registry().remove(c);
    }

    game::ModuleSet       set;
    engine::scene::Scene  scene;
    game::Services        services;
};

TEST_F(CoinModule, RegistersEverything) {
    EXPECT_EQ(set.names().size(), 1U);
    ASSERT_EQ(set.systems(game::Phase::pre_physics).size(), 1U);
    EXPECT_EQ(set.systems(game::Phase::pre_physics)[0].name, "coin_collector.spin");
    ASSERT_EQ(set.methods().size(), 1U);
    EXPECT_EQ(set.methods()[0].name, "coins.status");
    ASSERT_EQ(set.lua_modules().size(), 1U);
    const engine::scene::ComponentCodec* codec = engine::scene::component_registry().find("coin_spin");
    ASSERT_NE(codec, nullptr);
    ASSERT_TRUE(codec->schema);
    EXPECT_EQ((*codec->schema)["properties"]["spin_speed"]["maximum"], 3600.0);
}

TEST_F(CoinModule, SpinsAndBobsCoinsInPrePhysics) {
    entt::registry& r = scene.registry();
    const entt::entity coin = scene.create_entity("Coin");
    ASSERT_TRUE(engine::scene::apply_component(r, coin, "transform", {{"position", {2, 1, 0}}}, true));
    ASSERT_TRUE(engine::scene::apply_component(r, coin, "coin_spin", {{"spin_speed", 90}}, true));
    game::SystemContext context(scene, services);
    set.run(game::Phase::pre_physics, context, 1.0F);
    const auto& spin = r.get<coin_collector::CoinSpin>(coin);
    EXPECT_TRUE(spin.anchored);
    EXPECT_FLOAT_EQ(spin.angle, 90.0F);
    const glm::vec3 p(r.get<engine::scene::Transform>(coin).local[3]);
    EXPECT_FLOAT_EQ(p.x, 2.0F);
    EXPECT_NEAR(p.y, 1.0F + std::sin(spin.phase) * 0.15F, 1e-5F);
    // Strict decoding rejects out-of-range and read-only-as-unknown values.
    EXPECT_FALSE(engine::scene::apply_component(r, coin, "coin_spin", {{"spin_speed", 1e6}}, true));
    EXPECT_FALSE(engine::scene::apply_component(r, coin, "coin_spin", {{"spinspeed", 1}}, true));
}

TEST_F(CoinModule, MethodAndLuaFunction) {
    const entt::entity coin = scene.create_entity("Coin");
    scene.registry().emplace<coin_collector::CoinSpin>(coin);
    game::SystemContext context(scene, services);
    auto status = set.call("coins.status", context, json::object());
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ((*status)["remaining"], 1);
    auto bad = set.call("coins.status", context, {{"x", 1}});
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error().message.find("params.x: unknown field"), std::string::npos) << bad.error().message;

    const auto& fn = set.lua_modules()[0].functions[0];
    auto p = fn.fn(json::array({2, 8, 8.0}));
    ASSERT_TRUE(p) << p.error().message;
    EXPECT_NEAR((*p)[0].get<double>(), std::cos(0.5 * 3.14159265358979) * 8.0, 1e-5);
    EXPECT_EQ((*p)[1], 1.0);
    EXPECT_FALSE(fn.fn(json::array({9, 8, 8.0})));
    EXPECT_FALSE(fn.fn(json::array({"a"})));
}

} // namespace
