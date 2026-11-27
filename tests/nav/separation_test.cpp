// Agent-agent separation and the kinematic navmesh clamp (S1 N10).

#include <algorithm>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "nav/nav_fixtures.hpp"

namespace {

namespace s = engine::scene;
using quat::test::add_box;
using quat::test::kNavDt;
using quat::test::xz_distance;

// Regression (S1 N10): the separation push was renormalised away — an agent
// crowding another kept full speed and only turned.
TEST(Nav, SeparationSlowsAnAgentCrowdingAnother) {
    quat::test::NavWorld w;
    add_box(w.registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(20.0F, 0.5F, 20.0F));
    w.bake();
    const entt::entity mover = w.agent({0.0F, 0.0F, 0.0F}, 4.0F);
    w.agent({0.6F, 0.0F, 0.0F}); // standing right in front
    s::nav_agent_set_target(w.nav(mover), {10.0F, 0.0F, 0.0F});

    const glm::vec3 before = w.pos(mover);
    w.run(1, [] { return false; });
    const float moved = xz_distance(w.pos(mover), before);
    EXPECT_GT(moved, 0.0F);
    EXPECT_LT(moved, 0.75F * 4.0F * kNavDt) << "full speed despite the push";

    // Alone, the same agent walks at full speed.
    quat::test::NavWorld alone;
    add_box(alone.registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(20.0F, 0.5F, 20.0F));
    alone.bake();
    const entt::entity solo = alone.agent({0.0F, 0.0F, 0.0F}, 4.0F);
    s::nav_agent_set_target(alone.nav(solo), {10.0F, 0.0F, 0.0F});
    alone.run(1, [] { return false; });
    EXPECT_NEAR(xz_distance(alone.pos(solo), before), 4.0F * kNavDt, 1e-3F);
}

TEST(Nav, SeparationSlowsACharacterControllerAgent) {
    quat::test::NavWorld w;
    add_box(w.registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(20.0F, 0.5F, 20.0F));
    w.bake();
    const entt::entity mover = w.agent({0.0F, 0.9F, 0.0F});
    w.registry.emplace<s::CharacterController>(mover);
    w.agent({0.6F, 0.0F, 0.0F});
    s::nav_agent_set_target(w.nav(mover), {10.0F, 0.0F, 0.0F});
    w.run(1, [] { return false; });
    const glm::vec3 move = w.registry.get<s::CharacterController>(mover).move;
    EXPECT_GT(move.x, 0.0F);
    EXPECT_LT(glm::length(move), 0.75F);
}

// Regression (S1 N10): separation pushed kinematic agents off the navmesh.
TEST(Nav, KinematicAgentsStayOnTheMeshWhenPushed) {
    quat::test::NavWorld w;
    // A corridor 3 m wide (narrower once eroded by the agent radius).
    add_box(w.registry, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(10.0F, 0.5F, 1.5F));
    w.bake();
    auto edge = w.navmesh.nearest_point({0.0F, 0.0F, 3.0F}); // the +z border
    ASSERT_TRUE(edge.has_value());
    const float z = edge->z - 0.1F; // walk right along the border...
    const entt::entity walker = w.agent({-6.0F, 0.0F, z}, 3.0F);
    w.agent({0.0F, 0.0F, z - 0.8F}); // ...past an agent that pushes toward +z
    s::nav_agent_set_target(w.nav(walker), {6.0F, 0.0F, z});

    float worst = 0.0F;
    w.run(60 * 10, [&] {
        const glm::vec3 p = w.pos(walker);
        if (auto on = w.navmesh.nearest_point(p)) worst = std::max(worst, xz_distance(*on, p));
        return w.nav(walker).arrived;
    });
    EXPECT_TRUE(w.nav(walker).arrived);
    EXPECT_LT(worst, 0.02F) << "left the navmesh";
}

} // namespace
