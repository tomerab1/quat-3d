// Stuck detection for CharacterController agents (S1 N6): the controller
// pushes into a wall the navmesh does not know about. Runs Jolt on the CPU.

#include <algorithm>
#include <optional>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "engine/physics/physics_world.hpp"
#include "nav/nav_fixtures.hpp"

namespace {

namespace s = engine::scene;
using quat::test::add_box;
using quat::test::kNavDt;
using quat::test::xz_distance;

struct StuckWorld : quat::test::NavWorld {
    std::optional<engine::physics::PhysicsWorld> physics;
    entt::entity e = entt::null;
    entt::entity wall = entt::null;
    int max_failures_seen = 0;

    StuckWorld() {
        add_box(registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(15.0F, 0.5F, 15.0F));
        bake(); // the navmesh knows the floor only...
        // ...the wall arrives later (a door, a dropped crate): physics-only.
        wall = add_box(registry, glm::vec3(0.0F, 2.0F, 0.0F), glm::vec3(0.5F, 2.0F, 6.0F));
        (void)s::promote_collider_only_bodies(registry);
        auto created = engine::physics::PhysicsWorld::create();
        EXPECT_TRUE(created.has_value());
        if (created) physics = std::move(*created);

        e = quat::test::place(registry, glm::vec3(-5.0F, 1.0F, 0.0F));
        registry.emplace<s::CharacterController>(e); // radius 0.3, half height 0.6
        registry.emplace<s::NavAgent>(e).speed = 3.0F;
    }

    void step() {
        s::nav_agent_system(registry, navmesh, kNavDt);
        s::character_system(registry, *physics, kNavDt);
        s::physics_system(registry, *physics, kNavDt);
        max_failures_seen =
            std::max(max_failures_seen, static_cast<int>(nav(e).plan_failures));
    }
    template <typename Done>
    int run(int max_frames, Done done) {
        for (int i = 0; i < max_frames; ++i) {
            if (done()) return i;
            step();
        }
        return max_frames;
    }
};

TEST(Nav, StuckCharacterAgentReplansThenFails) {
    StuckWorld w;
    ASSERT_TRUE(w.physics.has_value());
    w.run(60, [] { return false; }); // land on the floor
    s::nav_agent_set_target(w.nav(w.e), {5.0F, 0.0F, 0.0F});

    const int frames = w.run(60 * 20, [&] { return !w.nav(w.e).active; });
    EXPECT_LT(frames, 60 * 20) << "pushed into the wall forever";
    EXPECT_TRUE(w.nav(w.e).failed);
    EXPECT_GE(w.max_failures_seen, 2) << "replanned before giving up";
    EXPECT_LT(w.pos(w.e).x, 0.0F) << "the wall held";
    EXPECT_EQ(w.registry.get<s::CharacterController>(w.e).move, glm::vec3(0.0F))
        << "a failed agent stops pushing";
}

TEST(Nav, StuckAgentRecoversWhenTheWayClears) {
    StuckWorld w;
    ASSERT_TRUE(w.physics.has_value());
    w.run(60, [] { return false; });
    s::nav_agent_set_target(w.nav(w.e), {5.0F, 0.0F, 0.0F});
    w.run(60 * 20, [&] { return w.nav(w.e).plan_failures >= 1; }); // first stuck replan
    ASSERT_TRUE(w.nav(w.e).active);
    w.registry.destroy(w.wall); // the door opens

    w.run(60 * 20, [&] { return !w.nav(w.e).active; });
    EXPECT_TRUE(w.nav(w.e).arrived);
    EXPECT_LT(xz_distance(w.pos(w.e), {5.0F, 0.0F, 0.0F}), 1.0F);
}

} // namespace
