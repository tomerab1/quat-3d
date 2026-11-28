// Joints (A10.1): Joint components become Jolt constraints through
// joint_system (run by physics_system). A hinge holds a door swinging within
// its limits, a distance joint keeps its length, motors drive hinges, a
// slider respects its limits, a fixed joint glues two bodies, removing a
// joint frees the body, and rebuilding a body rebuilds its joints.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
using engine::physics::PhysicsWorld;

constexpr float k_dt = PhysicsWorld::fixed_timestep;

class Joints : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));
    }

    entt::entity body(const glm::vec3& pos, const glm::vec3& half, s::BodyMotion motion,
                      s::ColliderShape shape = s::ColliderShape::box) {
        const entt::entity e = registry.create();
        auto& t = registry.emplace<s::Transform>(e);
        t.local = glm::translate(glm::mat4(1.0F), pos);
        t.world = t.local;
        registry.emplace<s::Collider>(e, shape, half, glm::vec3(0.0F));
        auto& rb = registry.emplace<s::RigidBody>(e);
        rb.motion = motion;
        rb.linear_damping = 0.0F;
        rb.angular_damping = 0.0F;
        return e;
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) s::physics_system(registry, *world, k_dt);
    }
    [[nodiscard]] glm::vec3 pos(entt::entity e) const {
        return glm::vec3(registry.get<s::Transform>(e).world[3]);
    }
    [[nodiscard]] glm::vec3 world_point(entt::entity e, const glm::vec3& local) const {
        return glm::vec3(registry.get<s::Transform>(e).world * glm::vec4(local, 1.0F));
    }
    [[nodiscard]] std::uint32_t rb(entt::entity e) const {
        return registry.get<s::RigidBody>(e).body;
    }
    [[nodiscard]] float joint_value(entt::entity e) const {
        return world->joint_position(registry.get<s::Joint>(e).constraint).value_or(1e9F);
    }

    std::optional<PhysicsWorld> world;
    entt::registry              registry;
};

TEST_F(Joints, AHingeHoldsADoorSwingingWithinItsLimits) {
    // A 1 m wide door hinged at its left edge (world x = 0) about +Y.
    const entt::entity door = body(glm::vec3(0.5F, 1.0F, 0.0F), glm::vec3(0.5F, 1.0F, 0.05F),
                                   s::BodyMotion::dynamic);
    auto& j = registry.emplace<s::Joint>(door);
    j.type = s::JointType::hinge;
    j.anchor = glm::vec3(-0.5F, 0.0F, 0.0F);
    j.axis = glm::vec3(0.0F, 1.0F, 0.0F);
    j.limits_enabled = true;
    j.limits = glm::vec2(-45.0F, 45.0F);
    frames(1);
    ASSERT_EQ(world->joint_count(), 1U);

    // Kick the free edge hard: it swings until the limit stops it.
    world->add_impulse(rb(door), glm::vec3(0.0F, 0.0F, -2.0F), glm::vec3(1.0F, 1.0F, 0.0F));
    float widest = 0.0F;
    for (int i = 0; i < 240; ++i) {
        frames(1);
        widest = std::max(widest, std::abs(glm::degrees(joint_value(door))));
        EXPECT_LT(glm::distance(world_point(door, j.anchor), glm::vec3(0.0F, 1.0F, 0.0F)), 0.05F)
            << "the door left its hinge at frame " << i;
    }
    EXPECT_GT(widest, 40.0F) << "the door barely moved";
    EXPECT_LT(widest, 48.0F) << "the door swung past its 45 degree limit";
}

TEST_F(Joints, ADistanceJointKeepsItsLength) {
    // A pendulum: a ball 2 m below a fixed point in the world.
    const entt::entity ball = body(glm::vec3(0.0F, 3.0F, 0.0F), glm::vec3(0.25F),
                                   s::BodyMotion::dynamic, s::ColliderShape::sphere);
    auto& j = registry.emplace<s::Joint>(ball);
    j.type = s::JointType::distance;
    j.target_anchor = glm::vec3(0.0F, 5.0F, 0.0F); // world space: no target
    frames(1);
    world->add_impulse(rb(ball), glm::vec3(3.0F, 0.0F, 0.0F));
    for (int i = 0; i < 360; ++i) {
        frames(1);
        EXPECT_NEAR(glm::distance(pos(ball), glm::vec3(0.0F, 5.0F, 0.0F)), 2.0F, 0.05F)
            << "frame " << i;
    }
    EXPECT_NEAR(joint_value(ball), 2.0F, 0.05F);
    EXPECT_GT(std::abs(pos(ball).x), 0.1F) << "the pendulum never swung";
}

TEST_F(Joints, RemovingTheJointFreesTheBody) {
    const entt::entity ball = body(glm::vec3(0.0F, 4.0F, 0.0F), glm::vec3(0.25F),
                                   s::BodyMotion::dynamic, s::ColliderShape::sphere);
    auto& j = registry.emplace<s::Joint>(ball);
    j.type = s::JointType::point;
    j.anchor = glm::vec3(0.0F, 1.0F, 0.0F); // hangs from (0, 5, 0)
    frames(120);
    EXPECT_NEAR(pos(ball).y, 4.0F, 0.05F) << "the joint did not hold the ball";
    ASSERT_TRUE(s::remove_component(registry, ball, "joint"));
    frames(1);
    EXPECT_EQ(world->joint_count(), 0U) << "the constraint outlived its component";
    frames(119);
    EXPECT_LT(pos(ball).y, 0.0F) << "the ball is still held";
}

TEST_F(Joints, AVelocityMotorTurnsAHinge) {
    const entt::entity wheel = body(glm::vec3(0.0F, 2.0F, 0.0F), glm::vec3(0.5F, 0.5F, 0.1F),
                                    s::BodyMotion::dynamic);
    registry.get<s::RigidBody>(wheel).gravity_factor = 0.0F;
    auto& j = registry.emplace<s::Joint>(wheel);
    j.type = s::JointType::hinge;
    j.axis = glm::vec3(0.0F, 0.0F, 1.0F);
    j.motor = s::JointMotorMode::velocity;
    j.motor_target = 90.0F; // deg/s
    frames(61);             // build + 0.5 s
    EXPECT_NEAR(glm::degrees(joint_value(wheel)), 45.0F, 5.0F);
}

TEST_F(Joints, ASliderStopsAtItsLimits) {
    const entt::entity sled = body(glm::vec3(0.0F, 2.0F, 0.0F), glm::vec3(0.25F),
                                   s::BodyMotion::dynamic);
    registry.get<s::RigidBody>(sled).gravity_factor = 0.0F;
    auto& j = registry.emplace<s::Joint>(sled);
    j.type = s::JointType::slider;
    j.axis = glm::vec3(1.0F, 0.0F, 0.0F);
    j.limits_enabled = true;
    j.limits = glm::vec2(0.0F, 1.0F);
    frames(1);
    world->add_impulse(rb(sled), glm::vec3(10.0F, 5.0F, 0.0F)); // also off-axis
    frames(120);
    EXPECT_NEAR(pos(sled).x, 1.0F, 0.05F);
    EXPECT_NEAR(pos(sled).y, 2.0F, 0.05F) << "left the slider axis";
}

TEST_F(Joints, AFixedJointGluesTwoBodies) {
    const entt::entity a = body(glm::vec3(0.0F, 5.0F, 0.0F), glm::vec3(0.25F), s::BodyMotion::dynamic);
    const entt::entity b = body(glm::vec3(1.0F, 5.0F, 0.0F), glm::vec3(0.25F), s::BodyMotion::dynamic);
    auto& j = registry.emplace<s::Joint>(a);
    j.type = s::JointType::fixed;
    j.target = b;
    frames(1);
    world->add_impulse(rb(b), glm::vec3(0.0F, 0.0F, 3.0F));
    frames(60);
    EXPECT_NEAR(glm::distance(pos(a), pos(b)), 1.0F, 0.02F);
    EXPECT_LT(pos(a).y, 5.0F) << "they should fall together";
}

TEST_F(Joints, RebuildingABodyRebuildsItsJoint) {
    const entt::entity ball = body(glm::vec3(0.0F, 4.0F, 0.0F), glm::vec3(0.25F),
                                   s::BodyMotion::dynamic, s::ColliderShape::sphere);
    auto& j = registry.emplace<s::Joint>(ball);
    j.type = s::JointType::point;
    j.anchor = glm::vec3(0.0F, 1.0F, 0.0F);
    frames(10);
    const std::uint32_t first = registry.get<s::Joint>(ball).constraint;
    // A collider edit rebuilds the body; its joint goes with it and comes back.
    ASSERT_OK(s::apply_component(registry, ball, "collider", {{"half_extents", {0.3, 0.3, 0.3}}},
                                 true));
    frames(1);
    EXPECT_EQ(world->joint_count(), 1U);
    EXPECT_NE(registry.get<s::Joint>(ball).constraint, first);
    frames(120);
    EXPECT_NEAR(pos(ball).y, 4.0F, 0.1F) << "the rebuilt joint does not hold";
}

TEST_F(Joints, NothingIsBuiltWithoutADynamicBodyOrForAMissingTarget) {
    const entt::entity wall = body(glm::vec3(0.0F), glm::vec3(1.0F), s::BodyMotion::static_body);
    registry.emplace<s::Joint>(wall).type = s::JointType::fixed; // static to the world
    const entt::entity crate = body(glm::vec3(5.0F, 1.0F, 0.0F), glm::vec3(0.5F),
                                    s::BodyMotion::dynamic);
    const entt::entity ghost = registry.create(); // no body
    registry.emplace<s::Joint>(crate, s::Joint{.type = s::JointType::point, .target = ghost});
    frames(2);
    EXPECT_EQ(world->joint_count(), 0U);
    EXPECT_EQ(registry.get<s::Joint>(crate).constraint, PhysicsWorld::invalid_body);
}

} // namespace
