// Game module systems in the frame (ROADMAP G17.1): WorldRuntime::simulate
// runs each phase at its place, only in play, with physics slices driving
// pre_physics / post_physics and Scene::tick hosting post_transform and
// pre_render.

#include <string>
#include <vector>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "app/play_mode.hpp"
#include "app/world_runtime.hpp"
#include "engine/game/module_set.hpp"
#include "engine/scene/component_registry.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace game = engine::game;

std::vector<std::string> g_log; // phases record their runs (test-only state)

void on_input(game::SystemContext&, float) { g_log.emplace_back("input"); }
void on_pre(game::SystemContext& ctx, float) {
    g_log.emplace_back(ctx.services().physics != nullptr ? "pre+physics" : "pre");
}
void on_post(game::SystemContext&, float) { g_log.emplace_back("post"); }
void on_transform(game::SystemContext&, float) { g_log.emplace_back("post_transform"); }
void on_render(game::SystemContext&, float) { g_log.emplace_back("pre_render"); }

std::expected<void, engine::core::Error> phases(game::Registrar& r) {
    r.system({.name = "t.render", .phase = game::Phase::pre_render, .run = &on_render});
    r.system({.name = "t.transform", .phase = game::Phase::post_transform, .run = &on_transform});
    r.system({.name = "t.post", .phase = game::Phase::post_physics, .run = &on_post});
    r.system({.name = "t.pre", .phase = game::Phase::pre_physics, .run = &on_pre});
    r.system({.name = "t.input", .phase = game::Phase::input, .run = &on_input});
    return {};
}

TEST(WorldRuntimeModules, PhasesRunInOrderOnlyInPlay) {
    engine::scene::ComponentRegistry components;
    const game::ModuleEntry modules[] = {{"phases", &phases}};
    auto set = game::ModuleSet::load(modules, components, {});
    ASSERT_OK(set);

    engine::scene::Scene scene;
    quat::app::WorldRuntime world;
    world.modules().bind(&*set, nullptr, {});
    g_log.clear();
    world.simulate(scene, 0, 1.0F / 60.0F, 1.0F / 60.0F, nullptr);
    EXPECT_TRUE(g_log.empty()) << "edit mode runs no gameplay";

    quat::app::PlayMode mode;
    *mode.request() = true;
    ASSERT_TRUE(world.apply_play(mode, scene));
    const quat::app::PlayStep step{};
    world.simulate(scene, 1, 1.0F / 60.0F, 1.0F / 60.0F, &step);
    EXPECT_EQ(g_log, (std::vector<std::string>{"input", "pre+physics", "post", "post_transform",
                                               "pre_render"}));
    *mode.request() = false;
    (void)world.apply_play(mode, scene);
}

} // namespace
