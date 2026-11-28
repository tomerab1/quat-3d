#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/dynamic_mesh.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace s = engine::scene;
namespace geo = engine::geometry;
using engine::physics::PhysicsWorld;

namespace {

// A 4 x 4 m floor quad facing +y, centred on the origin.
geo::MeshData floor_quad() {
    geo::MeshData m;
    m.vertices.resize(4);
    m.vertices[0].position = {-2, 0, -2};
    m.vertices[1].position = {2, 0, -2};
    m.vertices[2].position = {2, 0, 2};
    m.vertices[3].position = {-2, 0, 2};
    m.indices = {0, 2, 1, 0, 3, 2};
    return m;
}

} // namespace

TEST(DynamicMeshPhysics, StaticMeshColliderOwnedByTheEntity) {
    auto world = PhysicsWorld::create();
    ASSERT_OK(world);
    s::DynamicMeshStore store;
    ASSERT_OK(store.submit("floor", floor_quad()));
    entt::registry r;
    const entt::entity e = r.create();
    s::Transform t;
    t.world = glm::translate(glm::mat4{1.0F}, glm::vec3(10, 1, 0));
    r.emplace<s::Transform>(e, t);
    s::DynamicMesh d;
    d.mesh = "floor";
    d.collider = s::DynamicCollider::mesh;
    r.emplace<s::DynamicMesh>(e, d);

    s::dynamic_mesh_collider_system(r, *world, store);
    const std::uint32_t body = r.get<s::DynamicMesh>(e).body;
    ASSERT_NE(body, PhysicsWorld::invalid_body);
    EXPECT_EQ(world->body_owner(body), static_cast<std::uint32_t>(entt::to_integral(e)));

    // physics_system's owner sweep keeps it.
    s::physics_system(r, *world, 1.0F / 60.0F);
    EXPECT_EQ(r.get<s::DynamicMesh>(e).body, body);
    auto hit = world->cast_ray({10.5F, 5.0F, 0.5F}, {0.0F, -1.0F, 0.0F}, 10.0F);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->owner, static_cast<std::uint32_t>(entt::to_integral(e)));
    EXPECT_NEAR(hit->point.y, 1.0F, 1e-3F);

    // A new mesh version rebuilds the body; turning the collider off removes it.
    auto raised = floor_quad();
    for (auto& v : raised.vertices) v.position.y = 2.0F;
    ASSERT_OK(store.submit("floor", std::move(raised)));
    s::dynamic_mesh_collider_system(r, *world, store);
    hit = world->cast_ray({10.5F, 5.0F, 0.5F}, {0.0F, -1.0F, 0.0F}, 10.0F);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->point.y, 3.0F, 1e-3F);
    r.get<s::DynamicMesh>(e).collider = s::DynamicCollider::none;
    s::dynamic_mesh_collider_system(r, *world, store);
    EXPECT_EQ(r.get<s::DynamicMesh>(e).body, PhysicsWorld::invalid_body);
    EXPECT_FALSE(world->cast_ray({10.5F, 5.0F, 0.5F}, {0.0F, -1.0F, 0.0F}, 10.0F).has_value());
}
