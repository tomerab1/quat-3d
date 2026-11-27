// ECS <-> physics bridge (physics_system): bodies created from Collider +
// RigidBody + Transform.world, simulated poses written back (through the
// parent for child entities), world scale baked into shapes, and bodies swept
// when their entity or Collider goes away (S1 audit B6).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
using engine::physics::PhysicsWorld;

constexpr float k_dt = 1.0F / 60.0F;

class PhysicsSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));

        // Static floor entity: 100x2x100 box centred at y=-1 (top at y=0).
        const entt::entity floor = place(glm::vec3(0.0F, -1.0F, 0.0F));
        registry.emplace<s::Collider>(floor, s::ColliderShape::box,
                                      glm::vec3(50.0F, 1.0F, 50.0F), glm::vec3(0.0F));
        registry.emplace<s::RigidBody>(floor, s::BodyMotion::static_body, 1.0F,
                                       PhysicsWorld::invalid_body);
    }

    // Bodies are created from Transform.world (entities may be parented);
    // these bare registries run no transform system, so world is set here.
    entt::entity place(const glm::vec3& pos) {
        const entt::entity e = registry.create();
        auto& t = registry.emplace<s::Transform>(e);
        t.local = glm::translate(glm::mat4(1.0F), pos);
        t.world = t.local;
        return e;
    }

    // A dynamic sphere collider (authored radius 0.5) on `e`.
    void make_dynamic_ball(entt::entity e) {
        registry.emplace<s::Collider>(e, s::ColliderShape::sphere, glm::vec3(0.5F),
                                      glm::vec3(0.0F));
        registry.emplace<s::RigidBody>(e, s::BodyMotion::dynamic, 1.0F,
                                       PhysicsWorld::invalid_body);
    }

    // The parented-sphere case (glTF mesh children): the parent carries the
    // spawn offset (10, 5, 0), the child's local is identity.
    std::pair<entt::entity, entt::entity> add_parented_ball() {
        const entt::entity parent = place(glm::vec3(10.0F, 5.0F, 0.0F));
        const entt::entity child = registry.create();
        auto& child_t = registry.emplace<s::Transform>(child);
        child_t.local = glm::mat4(1.0F);
        child_t.world = registry.get<s::Transform>(parent).world; // as the transform system would
        registry.emplace<s::Parent>(child, parent);
        make_dynamic_ball(child);
        return {parent, child};
    }

    void step(int frames) {
        for (int i = 0; i < frames; ++i) s::physics_system(registry, *world, k_dt);
    }

    [[nodiscard]] glm::vec3 world_position(entt::entity e) const {
        return glm::vec3(registry.get<s::Transform>(e).world[3]);
    }

    [[nodiscard]] std::size_t bodies_owned_by(entt::entity e) const {
        std::size_t n = 0;
        for (const auto& ob : world->owned_bodies()) {
            n += ob.owner == static_cast<std::uint32_t>(entt::to_integral(e)) ? 1U : 0U;
        }
        return n;
    }

    std::optional<PhysicsWorld> world;
    entt::registry              registry;
};

TEST_F(PhysicsSystemTest, DynamicSphereEntityRestsOnTheFloor) {
    const entt::entity sphere = place(glm::vec3(0.0F, 5.0F, 0.0F));
    make_dynamic_ball(sphere);
    step(120);

    const float y = registry.get<s::Transform>(sphere).local[3].y;
    EXPECT_LE(y, 4.5F) << "sphere did not fall";
    EXPECT_GE(y, 0.0F) << "sphere fell through the floor";
    EXPECT_NEAR(y, 0.5F, 0.25F) << "sphere not resting on the floor";
}

// World scale 2 and authored radius 0.5 must simulate as a radius-1 body: it
// rests at y=1, matching what the editor's collider wireframe shows.
TEST_F(PhysicsSystemTest, WorldScaleIsBakedIntoTheColliderShape) {
    const entt::entity scaled = registry.create();
    auto& t = registry.emplace<s::Transform>(scaled);
    t.local = glm::translate(glm::mat4(1.0F), glm::vec3(20.0F, 5.0F, 0.0F)) *
              glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    t.world = t.local;
    make_dynamic_ball(scaled);
    step(120);

    EXPECT_NEAR(world_position(scaled).y, 1.0F, 0.25F)
        << "world scale not applied to the collider shape";
}

// The child body is created at its WORLD position (x=10), not its identity
// local, and falls there.
TEST_F(PhysicsSystemTest, ParentedBodySimulatesInWorldSpace) {
    const auto [parent, child] = add_parented_ball();
    step(120);

    const glm::vec3 p = world_position(child);
    EXPECT_NEAR(p.x, 10.0F, 0.5F);
    EXPECT_NEAR(p.y, 0.5F, 0.25F);
}

// The pulled pose is written back through the parent
// (local = inverse(parent_world) * pose), so local composes to the world pose.
TEST_F(PhysicsSystemTest, ParentedLocalPoseComposesThroughTheParent) {
    const auto [parent, child] = add_parented_ball();
    step(120);

    const glm::mat4 recomposed =
        registry.get<s::Transform>(parent).world * registry.get<s::Transform>(child).local;
    EXPECT_NEAR(recomposed[3].y, world_position(child).y, 0.01F);
}

// Destroying an entity, or removing its Collider, removes its Jolt body on
// the next tick.
TEST_F(PhysicsSystemTest, OrphanedBodiesAreRemoved) {
    const entt::entity sphere = place(glm::vec3(0.0F, 5.0F, 0.0F));
    make_dynamic_ball(sphere);
    const auto [parent, child] = add_parented_ball();
    step(120);

    const std::size_t before = world->owned_bodies().size();
    registry.destroy(sphere);
    registry.remove<s::Collider>(child);
    step(1);

    EXPECT_EQ(world->owned_bodies().size(), before - 2);
    EXPECT_EQ(bodies_owned_by(child), 0U);
    EXPECT_EQ(registry.get<s::RigidBody>(child).body, PhysicsWorld::invalid_body);
}

TEST_F(PhysicsSystemTest, ReaddedColliderGetsExactlyOneBody) {
    const auto [parent, child] = add_parented_ball();
    step(1);
    registry.remove<s::Collider>(child);
    step(1);
    ASSERT_EQ(bodies_owned_by(child), 0U);

    registry.emplace<s::Collider>(child, s::ColliderShape::sphere, glm::vec3(0.5F),
                                  glm::vec3(0.0F));
    step(2);
    EXPECT_EQ(bodies_owned_by(child), 1U);
}

} // namespace
