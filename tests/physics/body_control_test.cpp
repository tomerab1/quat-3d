// PhysicsWorld body control (A10.1): state reads, impulses, timed forces and
// torques, velocities, dynamic <-> kinematic switches, surface tuning and
// sleep control, all on deterministic fixed steps.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/physics/physics_world.hpp"
#include "support/expect_ok.hpp"

namespace {

using engine::physics::Layer;
using engine::physics::Motion;
using engine::physics::PhysicsWorld;

constexpr float k_step = PhysicsWorld::fixed_timestep;

class BodyControl : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));
    }

    std::uint32_t add_floor(float restitution = 0.0F) {
        PhysicsWorld::BodyParams p;
        p.shape = world->create_box(glm::vec3(50.0F, 1.0F, 50.0F));
        p.position = glm::vec3(0.0F, -1.0F, 0.0F);
        p.motion = Motion::static_body;
        p.layer = Layer::static_body;
        p.restitution = restitution;
        return world->add_body(p);
    }

    // A 1 kg ball floating in the air (gravity factor 0 unless given).
    std::uint32_t add_ball(const glm::vec3& pos, float gravity_factor = 0.0F,
                           float restitution = 0.0F) {
        PhysicsWorld::BodyParams p;
        p.shape = world->create_sphere(0.5F);
        p.position = pos;
        p.mass = 1.0F;
        p.gravity_factor = gravity_factor;
        p.restitution = restitution;
        p.linear_damping = 0.0F;
        p.angular_damping = 0.0F;
        return world->add_body(p);
    }

    // Exactly `steps` fixed steps.
    void steps(int n) {
        for (int i = 0; i < n; ++i) world->update(k_step);
    }

    std::optional<PhysicsWorld> world;
};

TEST_F(BodyControl, StateReportsWhatTheBodyWasBuiltWith) {
    PhysicsWorld::BodyParams p;
    p.shape = world->create_box(glm::vec3(0.5F));
    p.position = glm::vec3(1.0F, 2.0F, 3.0F);
    p.mass = 4.0F;
    p.friction = 0.7F;
    p.restitution = 0.3F;
    p.gravity_factor = 0.5F;
    p.linear_damping = 0.2F;
    const std::uint32_t box = world->add_body(p);
    const auto s = world->body_state(box);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->motion, Motion::dynamic);
    EXPECT_EQ(s->layer, Layer::dynamic);
    EXPECT_TRUE(s->awake);
    EXPECT_NEAR(s->mass, 4.0F, 1e-4F);
    EXPECT_NEAR(s->friction, 0.7F, 1e-6F);
    EXPECT_NEAR(s->restitution, 0.3F, 1e-6F);
    EXPECT_NEAR(s->gravity_factor, 0.5F, 1e-6F);
    EXPECT_NEAR(s->linear_damping, 0.2F, 1e-6F);
    EXPECT_NEAR(s->position.y, 2.0F, 1e-5F);
    world->remove_body(box);
    EXPECT_FALSE(world->body_state(box).has_value());
}

TEST_F(BodyControl, AnImpulseChangesVelocityAtOnce) {
    const std::uint32_t ball = add_ball(glm::vec3(0.0F, 5.0F, 0.0F));
    world->add_impulse(ball, glm::vec3(0.0F, 0.0F, 5.0F)); // 1 kg: +5 m/s
    EXPECT_NEAR(world->linear_velocity(ball).z, 5.0F, 1e-4F);
    steps(120);
    EXPECT_NEAR(world->body_state(ball)->position.z, 5.0F, 0.05F);
}

TEST_F(BodyControl, AnImpulseOffCentreAlsoSpins) {
    const std::uint32_t ball = add_ball(glm::vec3(0.0F, 5.0F, 0.0F));
    world->add_impulse(ball, glm::vec3(0.0F, 0.0F, 1.0F), glm::vec3(0.0F, 5.5F, 0.0F));
    EXPECT_GT(glm::length(world->angular_velocity(ball)), 0.5F);
}

TEST_F(BodyControl, AForceActsForItsDurationOnly) {
    const std::uint32_t ball = add_ball(glm::vec3(0.0F, 5.0F, 0.0F));
    world->add_force(ball, glm::vec3(10.0F, 0.0F, 0.0F), 0.5F); // 10 N * 0.5 s = 5 N*s
    steps(120); // 1 s: pushed for the first half only
    EXPECT_NEAR(world->linear_velocity(ball).x, 5.0F, 0.15F);
    steps(60);
    EXPECT_NEAR(world->linear_velocity(ball).x, 5.0F, 0.15F) << "the force outlived its duration";
}

// Forces act in whole fixed steps: a duration rounds to the nearest step
// count (at least one), and the body gets exactly force * steps * dt.
TEST_F(BodyControl, ForceDurationsRoundToWholeSteps) {
    EXPECT_EQ(PhysicsWorld::force_steps(0.001F), 1) << "shorter than a step still acts once";
    EXPECT_EQ(PhysicsWorld::force_steps(k_step * 3.4F), 3);
    EXPECT_EQ(PhysicsWorld::force_steps(k_step * 3.6F), 4);
    EXPECT_EQ(PhysicsWorld::force_steps(-1.0F), 1);
    EXPECT_EQ(PhysicsWorld::force_steps(1.0e9F), 1200) << "clamped to max_force_duration";
    EXPECT_FLOAT_EQ(PhysicsWorld::force_duration(k_step * 3.4F), 3.0F * k_step);

    const std::uint32_t ball = add_ball(glm::vec3(0.0F, 5.0F, 0.0F));
    world->add_force(ball, glm::vec3(12.0F, 0.0F, 0.0F), k_step * 2.9F); // 3 steps
    steps(10);
    EXPECT_NEAR(world->linear_velocity(ball).x, 12.0F * 3.0F * k_step, 1e-4F)
        << "the force must act for exactly the reported number of steps";
}

TEST_F(BodyControl, TorqueAndAngularImpulseSpinTheBody) {
    const std::uint32_t a = add_ball(glm::vec3(-5.0F, 5.0F, 0.0F));
    const std::uint32_t b = add_ball(glm::vec3(5.0F, 5.0F, 0.0F));
    world->add_angular_impulse(a, glm::vec3(0.0F, 1.0F, 0.0F));
    world->add_torque(b, glm::vec3(0.0F, 1.0F, 0.0F), 0.25F);
    steps(60);
    EXPECT_GT(world->angular_velocity(a).y, 1.0F);
    EXPECT_GT(world->angular_velocity(b).y, 1.0F);
}

TEST_F(BodyControl, VelocitiesCanBeSet) {
    const std::uint32_t ball = add_ball(glm::vec3(0.0F, 5.0F, 0.0F));
    world->set_linear_velocity(ball, glm::vec3(1.0F, 2.0F, 3.0F));
    world->set_angular_velocity(ball, glm::vec3(0.0F, 3.0F, 0.0F));
    EXPECT_EQ(world->linear_velocity(ball), glm::vec3(1.0F, 2.0F, 3.0F));
    EXPECT_NEAR(world->angular_velocity(ball).y, 3.0F, 1e-5F);
}

TEST_F(BodyControl, StaticBodiesIgnoreControl) {
    const std::uint32_t floor = add_floor();
    EXPECT_FALSE(world->is_dynamic(floor));
    world->add_impulse(floor, glm::vec3(100.0F));
    world->set_linear_velocity(floor, glm::vec3(1.0F));
    EXPECT_EQ(world->linear_velocity(floor), glm::vec3(0.0F));
    EXPECT_FALSE(world->set_motion(floor, Motion::dynamic)) << "static bodies are rebuilt instead";
}

TEST_F(BodyControl, SwitchingToKinematicStopsTheFall) {
    const std::uint32_t ball = add_ball(glm::vec3(0.0F, 5.0F, 0.0F), 1.0F);
    steps(30);
    ASSERT_TRUE(world->set_motion(ball, Motion::kinematic));
    world->set_linear_velocity(ball, glm::vec3(0.0F)); // ignored: no longer dynamic
    const auto s = world->body_state(ball);
    EXPECT_EQ(s->motion, Motion::kinematic);
    EXPECT_EQ(s->mass, 0.0F);
    ASSERT_TRUE(world->set_motion(ball, Motion::dynamic));
    EXPECT_TRUE(world->is_dynamic(ball));
}

TEST_F(BodyControl, RestitutionMakesABallBounce) {
    add_floor(); // Jolt combines restitution with max(): the floor stays at 0
    const std::uint32_t dull = add_ball(glm::vec3(-3.0F, 3.0F, 0.0F), 1.0F, 0.0F);
    const std::uint32_t bouncy = add_ball(glm::vec3(3.0F, 3.0F, 0.0F), 1.0F, 0.0F);
    world->set_restitution(bouncy, 0.9F);
    float bouncy_peak = 0.0F;
    float dull_peak = 0.0F;
    steps(110); // both land around 0.64 s
    for (int i = 0; i < 60; ++i) {
        steps(1);
        bouncy_peak = std::max(bouncy_peak, world->body_state(bouncy)->position.y);
        dull_peak = std::max(dull_peak, world->body_state(dull)->position.y);
    }
    EXPECT_GT(bouncy_peak, 1.5F);
    EXPECT_LT(dull_peak, 0.7F);
}

TEST_F(BodyControl, GravityFactorAndDampingApply) {
    const std::uint32_t ball = add_ball(glm::vec3(0.0F, 50.0F, 0.0F), 1.0F);
    world->set_gravity_factor(ball, 0.0F);
    steps(60);
    EXPECT_NEAR(world->linear_velocity(ball).y, 0.0F, 1e-4F);
    world->set_linear_velocity(ball, glm::vec3(10.0F, 0.0F, 0.0F));
    world->set_damping(ball, 2.0F, 0.0F);
    steps(120);
    EXPECT_LT(world->linear_velocity(ball).x, 2.0F);
    EXPECT_NEAR(world->body_state(ball)->linear_damping, 2.0F, 1e-6F);
}

TEST_F(BodyControl, BodiesCanBePutToSleepAndWoken) {
    const std::uint32_t ball = add_ball(glm::vec3(0.0F, 5.0F, 0.0F), 1.0F);
    world->set_awake(ball, false);
    EXPECT_FALSE(world->body_state(ball)->awake);
    steps(30);
    EXPECT_NEAR(world->body_state(ball)->position.y, 5.0F, 1e-5F) << "a sleeping body moved";
    world->set_awake(ball, true);
    steps(30);
    EXPECT_LT(world->body_state(ball)->position.y, 5.0F);
}

} // namespace
