// PatrolRoute feeding together with user orders (Ctrl+click = command,
// inspector Stop/Go) — S1 N5.

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "nav/nav_fixtures.hpp"

namespace {

namespace s = engine::scene;
using quat::test::xz_distance;

struct Patrol : quat::test::NavWorld {
    entt::entity e = entt::null;

    Patrol() {
        quat::test::add_box(registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(20.0F, 0.5F, 20.0F));
        bake();
        e = agent({0.0F, 0.0F, 0.0F}, 8.0F);
    }
    s::PatrolRoute& route(std::vector<glm::vec3> points, bool loop = true) {
        auto& r = registry.emplace_or_replace<s::PatrolRoute>(e);
        r.points = std::move(points);
        r.loop = loop;
        return r;
    }
    bool near(const glm::vec3& p) { return xz_distance(pos(e), p) < 0.8F; }
};

const glm::vec3 kA{10.0F, 0.0F, 0.0F};
const glm::vec3 kB{-10.0F, 0.0F, 0.0F};
const glm::vec3 kC{0.0F, 0.0F, 12.0F};

// Regression (S1 N5): after a Ctrl+click the agent "arrived" somewhere off
// the route and the route never fed it again.
TEST(Nav, PatrolResumesAfterACommandedMove) {
    Patrol w;
    auto& route = w.route({kA, kB});
    w.run(20, [] { return false; }); // heading for A
    ASSERT_TRUE(w.nav(w.e).active);
    s::nav_agent_command(w.nav(w.e), kC); // Ctrl+click
    EXPECT_LT(w.run(60 * 10, [&] { return w.near(kC); }), 60 * 10) << "the command wins";
    EXPECT_EQ(route.next, 0U) << "A was not reached, so the route still points at it";
    EXPECT_LT(w.run(60 * 10, [&] { return w.near(kA); }), 60 * 10) << "route resumes at A";
    EXPECT_LT(w.run(60 * 10, [&] { return w.near(kB); }), 60 * 10) << "...then B";
}

// Regression (S1 N5): Stop could not stop a patroller (the feed restarted it).
TEST(Nav, StopHoldsAPatrollerAndGoResumesIt) {
    Patrol w;
    w.route({kA, kB});
    w.run(30, [] { return false; });
    s::nav_agent_stop(w.nav(w.e));
    const glm::vec3 held = w.pos(w.e);
    w.run(120, [] { return false; });
    EXPECT_LT(xz_distance(w.pos(w.e), held), 1e-3F);
    EXPECT_FALSE(w.nav(w.e).active);

    s::nav_agent_resume(w.registry, w.e);
    EXPECT_LT(w.run(60 * 10, [&] { return w.near(kA); }), 60 * 10);
}

// Without a route, Go walks to the agent's current target.
TEST(Nav, GoWithoutARouteWalksToTheTarget) {
    Patrol w;
    w.nav(w.e).target = kC;
    s::nav_agent_resume(w.registry, w.e);
    EXPECT_LT(w.run(60 * 10, [&] { return w.nav(w.e).arrived; }), 60 * 10);
    EXPECT_TRUE(w.near(kC));
}

// Regression (S1 N5): a first Shift+click point added after the agent had
// arrived somewhere was ignored.
TEST(Nav, PointAddedToAnIdleAgentIsWalked) {
    Patrol w;
    s::nav_agent_command(w.nav(w.e), kC);
    w.run(60 * 10, [&] { return w.nav(w.e).arrived; });
    ASSERT_TRUE(w.nav(w.e).arrived);
    w.route({}).points.push_back(kA); // Shift+click
    EXPECT_LT(w.run(60 * 10, [&] { return w.near(kA); }), 60 * 10);
}

TEST(Nav, OneWayRouteFinishesAndContinuesWhenExtended) {
    Patrol w;
    auto& route = w.route({kA}, /*loop=*/false);
    w.run(60 * 10, [&] { return route.finished; });
    ASSERT_TRUE(route.finished);
    EXPECT_TRUE(w.near(kA));

    // A command after the end does not send the agent back to the route.
    s::nav_agent_command(w.nav(w.e), kC);
    w.run(60 * 10, [&] { return w.nav(w.e).arrived; });
    w.run(60, [] { return false; });
    EXPECT_TRUE(w.near(kC));

    route.points.push_back(kB); // extending the route carries on
    EXPECT_LT(w.run(60 * 10, [&] { return w.near(kB); }), 60 * 10);
}

TEST(Nav, OnePointLoopSettles) {
    Patrol w;
    w.route({kA});
    w.run(60 * 10, [&] { return w.nav(w.e).arrived; });
    ASSERT_TRUE(w.nav(w.e).arrived);
    for (int i = 0; i < 30; ++i) {
        w.run(1, [] { return false; });
        EXPECT_TRUE(w.nav(w.e).arrived) << "frame " << i << ": replanned in place";
    }
}

} // namespace
