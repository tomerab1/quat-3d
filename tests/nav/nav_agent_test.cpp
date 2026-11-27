// NavAgent path following (nav_agent_system) on navmeshes baked from box
// colliders.

#include <algorithm>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "nav/nav_fixtures.hpp"

namespace {

namespace s = engine::scene;
using quat::test::add_box;
using quat::test::position_of;
using quat::test::xz_distance;
using World = quat::test::NavWorld;


// 40 x 40 floor (top y = 0) with a wall across z = 0 leaving a gap at +x.
void walled_plate(World& w) {
    add_box(w.registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(20.0F, 0.5F, 20.0F));
    add_box(w.registry, glm::vec3(-4.0F, 1.5F, 0.0F), glm::vec3(16.0F, 1.5F, 0.5F));
    w.bake();
}

TEST(Nav, AgentDetoursThroughTheGap) {
    World w;
    walled_plate(w);
    const entt::entity e = w.agent({-10.0F, 0.0F, -10.0F});
    s::nav_agent_set_target(w.nav(e), {-10.0F, 0.0F, 10.0F});
    float max_x = -1e9F;
    w.run(60 * 60, [&] {
        max_x = std::max(max_x, position_of(w.registry, e).x);
        return !w.nav(e).active;
    });
    EXPECT_TRUE(w.nav(e).arrived);
    EXPECT_FALSE(w.nav(e).failed);
    EXPECT_LT(xz_distance(position_of(w.registry, e), {-10.0F, 0.0F, 10.0F}), 1.0F);
    EXPECT_GT(max_x, 10.0F) << "walked through the wall instead of the gap";
}

TEST(Nav, PatrolRouteLoopsAndSkipsUnreachablePoints) {
    World w;
    add_box(w.registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(20.0F, 0.5F, 20.0F));
    w.bake();
    const entt::entity e = w.agent({0.0F, 0.0F, 0.0F}, 8.0F);
    auto& route = w.registry.emplace<s::PatrolRoute>(e);
    // The third point floats far off the mesh: the route skips it.
    route.points = {{10.0F, 0.0F, 0.0F}, {-10.0F, 0.0F, 0.0F}, {0.0F, 50.0F, 0.0F}};

    bool reached_a = false, reached_b = false, wrapped = false;
    w.run(60 * 40, [&] {
        const glm::vec3 pos = position_of(w.registry, e);
        reached_a = reached_a || glm::distance(pos, route.points[0]) < 1.0F;
        reached_b = reached_b || (reached_a && glm::distance(pos, route.points[1]) < 1.0F);
        wrapped = wrapped || (reached_b && route.next == 0);
        return wrapped;
    });
    EXPECT_TRUE(reached_a);
    EXPECT_TRUE(reached_b);
    EXPECT_TRUE(wrapped) << "the route did not loop past the unreachable point";
}

// Regression (S1 N4): an unreachable target used to end as "arrived".
TEST(Nav, UnreachableTargetFails) {
    World w;
    // Two floor islands separated by a 4 m gap.
    add_box(w.registry, glm::vec3(-12.0F, -0.5F, 0.0F), glm::vec3(10.0F, 0.5F, 10.0F));
    add_box(w.registry, glm::vec3(12.0F, -0.5F, 0.0F), glm::vec3(10.0F, 0.5F, 10.0F));
    w.bake();
    const entt::entity e = w.agent({-12.0F, 0.0F, 0.0F});
    s::nav_agent_set_target(w.nav(e), {12.0F, 0.0F, 0.0F});
    const int frames = w.run(60 * 20, [&] { return !w.nav(e).active; });
    EXPECT_LT(frames, 60 * 20) << "never gave up";
    EXPECT_TRUE(w.nav(e).failed);
    EXPECT_FALSE(w.nav(e).arrived);
    // It walked to the closest reachable point first (the island's edge).
    EXPECT_GT(position_of(w.registry, e).x, -3.5F);
}

TEST(Nav, OffMeshTargetFailsAfterRetries) {
    World w;
    add_box(w.registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(10.0F, 0.5F, 10.0F));
    w.bake();
    const entt::entity e = w.agent({0.0F, 0.0F, 0.0F});
    s::nav_agent_set_target(w.nav(e), {0.0F, 60.0F, 0.0F});
    const int frames = w.run(60 * 10, [&] { return w.nav(e).failed; });
    EXPECT_TRUE(w.nav(e).failed);
    EXPECT_GT(frames, 60) << "retries on a cooldown before giving up";
    EXPECT_FALSE(w.nav(e).active);
}

// A target just off the mesh (inside a wall's clearance) snaps to the
// closest on-mesh point: arriving there is success, not failure.
TEST(Nav, TargetBesideAWallArrivesAtTheClosestPoint) {
    World w;
    walled_plate(w);
    const entt::entity e = w.agent({-10.0F, 0.0F, -10.0F});
    s::nav_agent_set_target(w.nav(e), {-10.0F, 0.0F, -0.6F}); // against the wall
    w.run(60 * 20, [&] { return !w.nav(e).active; });
    EXPECT_TRUE(w.nav(e).arrived);
    EXPECT_FALSE(w.nav(e).failed);
}

} // namespace
