// Navmesh bake input (collect_nav_geometry) and bakes through it.

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include "nav/nav_fixtures.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
using quat::test::add_box;
using quat::test::bake;

// Regression (S1 N2): box triangles were wound so the top faced down — box
// tops were unwalkable and the bottoms walkable. Every face must point out.
TEST(Nav, BoxColliderTrianglesFaceOutward) {
    entt::registry r;
    const entt::entity box = add_box(r, glm::vec3(3.0F, 1.0F, -2.0F), glm::vec3(2.0F, 0.5F, 1.0F));
    // Rotated and scaled: the world matrix must not flip the winding.
    auto& t = r.get<s::Transform>(box);
    t.world = glm::rotate(t.world, 0.6F, glm::vec3(0.0F, 1.0F, 0.0F));
    t.world = glm::scale(t.world, glm::vec3(1.5F, 2.0F, 0.5F));

    std::vector<glm::vec3> verts;
    std::vector<std::uint32_t> idx;
    s::collect_nav_geometry(r, nullptr, glm::vec3(0.0F), verts, idx);
    ASSERT_EQ(verts.size(), 8U);
    ASSERT_EQ(idx.size(), 36U);

    const glm::vec3 centre(t.world[3]);
    int up_facing = 0;
    for (std::size_t i = 0; i < idx.size(); i += 3) {
        const glm::vec3 a = verts[idx[i]];
        const glm::vec3 b = verts[idx[i + 1]];
        const glm::vec3 c = verts[idx[i + 2]];
        // Recast's convention (rcMarkWalkableTriangles): n = (b - a) x (c - a).
        const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        const glm::vec3 out = (a + b + c) / 3.0F - centre;
        EXPECT_GT(glm::dot(n, out), 0.0F) << "triangle " << i / 3 << " faces into the box";
        if (n.y > 0.99F) ++up_facing;
    }
    EXPECT_EQ(up_facing, 2) << "exactly the two top triangles face up";
}

// A raised box floor with a box obstacle on it, baked through the editor's
// geometry path: the route detours around the obstacle on the floor's TOP.
TEST(Nav, BakeBoxFloorRoutesAroundBoxObstacle) {
    entt::registry r;
    constexpr float floor_top = 1.0F;
    add_box(r, glm::vec3(0.0F, floor_top - 0.5F, 0.0F), glm::vec3(20.0F, 0.5F, 20.0F));
    // Wall across x = 0, 3 m tall, spanning z in [-8, 8].
    add_box(r, glm::vec3(0.0F, floor_top + 1.5F, 0.0F), glm::vec3(1.0F, 1.5F, 8.0F));

    auto mesh = bake(r);
    ASSERT_OK(mesh);
    auto path = mesh->find_path({-10.0F, floor_top, 0.0F}, {10.0F, floor_top, 0.0F});
    ASSERT_OK(path);
    ASSERT_GE(path->size(), 3U) << "a straight line would cross the wall";

    float max_abs_z = 0.0F;
    for (const glm::vec3& p : *path) {
        max_abs_z = std::max(max_abs_z, std::abs(p.z));
        EXPECT_NEAR(p.y, floor_top, 0.35F) << "path left the floor's top surface";
    }
    EXPECT_GT(max_abs_z, 8.0F) << "path must go around the wall's end";
    EXPECT_NEAR(path->back().x, 10.0F, 0.5F);

    // The wall's top (3 m up, 2 m wide) is not reachable from the floor and
    // the floor's underside (y = 0) is not walkable at all.
    auto under = mesh->nearest_point({-10.0F, 0.0F, 0.0F});
    ASSERT_OK(under);
    EXPECT_NEAR(under->y, floor_top, 0.35F);
}

// Regression (S1 N12): an agent's own collider is not level geometry — it
// used to be baked as an obstacle (a hole under the agent).
TEST(Nav, BakeSkipsAgentAndCharacterColliders) {
    entt::registry r;
    add_box(r, glm::vec3(0.0F, -0.5F, 0.0F), glm::vec3(10.0F, 0.5F, 10.0F));
    const entt::entity agent = add_box(r, glm::vec3(2.0F, 0.5F, 0.0F), glm::vec3(0.5F));
    r.emplace<s::NavAgent>(agent);
    const entt::entity character = add_box(r, glm::vec3(-2.0F, 0.5F, 0.0F), glm::vec3(0.5F));
    r.emplace<s::CharacterController>(character);

    std::vector<glm::vec3> verts;
    std::vector<std::uint32_t> idx;
    s::collect_nav_geometry(r, nullptr, glm::vec3(0.0F), verts, idx);
    EXPECT_EQ(idx.size(), 36U) << "only the floor box is level geometry";

    auto mesh = bake(r);
    ASSERT_OK(mesh);
    auto under_agent = mesh->nearest_point({2.0F, 0.0F, 0.0F});
    ASSERT_OK(under_agent);
    EXPECT_LT(quat::test::xz_distance(*under_agent, {2.0F, 0.0F, 0.0F}), 0.05F)
        << "the agent's footprint must stay walkable";
}

// Regression (S1 N12): Play's collider-only promotion made agents static walls.
TEST(Nav, PlayPromotionKeepsAgentsMoving) {
    entt::registry r;
    const entt::entity wall = add_box(r, glm::vec3(0.0F), glm::vec3(1.0F));
    const entt::entity agent = add_box(r, glm::vec3(3.0F, 0.0F, 0.0F), glm::vec3(0.4F));
    r.emplace<s::NavAgent>(agent);
    const entt::entity character = add_box(r, glm::vec3(-3.0F, 0.0F, 0.0F), glm::vec3(0.4F));
    r.emplace<s::CharacterController>(character);
    const entt::entity dynamic = add_box(r, glm::vec3(0.0F, 5.0F, 0.0F), glm::vec3(0.5F));
    r.emplace<s::RigidBody>(dynamic, s::BodyMotion::dynamic, 2.0F);

    EXPECT_EQ(s::promote_collider_only_bodies(r), 2U);
    ASSERT_TRUE(r.all_of<s::RigidBody>(wall));
    EXPECT_EQ(r.get<s::RigidBody>(wall).motion, s::BodyMotion::static_body);
    ASSERT_TRUE(r.all_of<s::RigidBody>(agent));
    EXPECT_EQ(r.get<s::RigidBody>(agent).motion, s::BodyMotion::kinematic);
    EXPECT_FALSE(r.all_of<s::RigidBody>(character));
    EXPECT_EQ(r.get<s::RigidBody>(dynamic).motion, s::BodyMotion::dynamic);
}

} // namespace
