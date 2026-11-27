// G7: the shipped Coin Collector demo (assets/scenes/coin_collector.json +
// assets/scripts/coin_collector/) plays end to end: the scene file decodes
// strictly, the game spawns its coins, the player walks into one under
// simulated input, the coin reports to the game and removes itself.

#include <filesystem>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/script/script_input.hpp"
#include "engine/script/script_source.hpp"
#include "script/script_fixture.hpp"

namespace quat::test {
namespace {

namespace fs = std::filesystem;
using namespace engine::scene;

const fs::path k_root = QUAT_PROJECT_ROOT;

TEST(ScriptDemo, EveryShippedScriptCompiles) {
    const fs::path scripts = k_root / "assets" / "scripts";
    const auto files = engine::script::list_scripts(scripts);
    ASSERT_FALSE(files.empty());
    for (const std::string& rel : files) {
        auto source = engine::script::read_script(scripts / rel);
        ASSERT_OK(source) << rel;
        EXPECT_OK(engine::script::check_syntax(*source, rel)) << rel;
    }
}

TEST(ScriptDemo, CoinCollectorPlays) {
    std::ifstream in(k_root / "assets" / "scenes" / "coin_collector.json");
    ASSERT_TRUE(in) << "assets/scenes/coin_collector.json";
    const nlohmann::json doc = nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(in), {}),
                                                     nullptr, false);
    ASSERT_TRUE(doc.is_object());

    Scene scene;
    entt::registry& r = scene.registry();
    entt::entity player = entt::null;
    for (const nlohmann::json& je : doc["entities"]) {
        const entt::entity e = scene.create_entity(je["name"].get<std::string>());
        for (const auto& [key, value] : je.items()) {
            if (key == "name" || key == "parent") continue;
            ASSERT_OK(apply_component(r, e, key, value, /*strict=*/true)) << key;
        }
        if (je["name"] == "Player") player = e;
    }
    ASSERT_NE(player, entt::entity{entt::null});
    scene.tick(0.0F);
    (void)promote_collider_only_bodies(r); // as the editor's Play does

    auto world = engine::physics::PhysicsWorld::create();
    ASSERT_OK(world);
    engine::script::ScriptRuntime runtime(
        engine::script::ScriptRuntime::Options{k_root / "assets" / "scripts", {}, false});
    engine::script::ScriptInput input;
    engine::script::ScriptServices services;
    services.physics = &*world;
    services.input = &input;
    runtime.set_services(services);
    ASSERT_OK(runtime.start(scene));

    const std::uint64_t mark = engine::core::log_ring().last_seq();
    const std::size_t right = *engine::script::key_from_name("d");
    int coins_at_start = -1;
    for (int frame = 0; frame < 300; ++frame) {
        input.begin_frame();
        if (frame > 10) input.keys.set(right); // walk towards the coin on +X
        const float dt = 1.0F / 60.0F;
        scene.tick(0.0F);
        character_system(r, *world, dt);
        physics_system(r, *world, dt);
        runtime.dispatch_triggers(*world);
        runtime.update(dt);
        if (frame == 5) coins_at_start = static_cast<int>(r.view<Primitive>().size());
    }

    engine::core::LogQuery q;
    q.since_seq = mark;
    q.category = "script";
    q.limit = 1000;
    std::string log;
    for (const auto& line : engine::core::log_ring().query(q).lines) log += line.message + "\n";
    EXPECT_NE(log.find("Collect all 8 coins!"), std::string::npos) << log;
    EXPECT_NE(log.find("Score: 1 (1/8 coins)"), std::string::npos) << log;
    EXPECT_EQ(log.find("stopped:"), std::string::npos) << "no script failed:\n" << log;
    EXPECT_EQ(static_cast<int>(r.view<Primitive>().size()), coins_at_start - 1)
        << "the collected coin destroyed itself";
    for (const auto& s : runtime.statuses()) {
        EXPECT_EQ(s.state, engine::script::ScriptStatus::State::ok) << s.path << ": " << s.message;
    }
}

} // namespace
} // namespace quat::test
