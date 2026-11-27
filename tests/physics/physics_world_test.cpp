// PhysicsWorld (Jolt wrapper) on its own, without the ECS: the fixed-step
// accumulator, rigid bodies settling on a static floor, and sensor trigger
// events.

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

constexpr float k_dt = 1.0F / 60.0F;

class PhysicsWorldTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));
    }

    // Static floor: a 100x2x100 box centred at y=-1, so its top is at y=0.
    void add_floor() {
        PhysicsWorld::BodyParams floor;
        floor.shape = world->create_box(glm::vec3(50.0F, 1.0F, 50.0F));
        floor.position = glm::vec3(0.0F, -1.0F, 0.0F);
        floor.motion = Motion::static_body;
        floor.layer = Layer::static_body;
        ASSERT_NE(world->add_body(floor), PhysicsWorld::invalid_body);
    }

    // Dynamic sphere of radius 0.5 at `position`, with the shape's own mass.
    [[nodiscard]] std::uint32_t add_ball(const glm::vec3& position) {
        PhysicsWorld::BodyParams ball;
        ball.shape = world->create_sphere(0.5F);
        ball.position = position;
        ball.motion = Motion::dynamic;
        ball.mass = 0.0F;
        return world->add_body(ball);
    }

    [[nodiscard]] glm::vec3 position_of(std::uint32_t body) const {
        glm::vec3 p(0.0F);
        glm::quat q(1.0F, 0.0F, 0.0F, 0.0F);
        world->body_transform(body, p, q);
        return p;
    }

    void step(int frames) {
        for (int i = 0; i < frames; ++i) world->update(k_dt);
    }

    std::optional<PhysicsWorld> world;
};

// A hitch longer than the catch-up cap must not leave a backlog that makes
// later frames run fast (S1 audit B7).
TEST_F(PhysicsWorldTest, HitchDoesNotLeaveABacklog) {
    add_floor();
    ASSERT_NE(add_ball(glm::vec3(0.0F, 5.0F, 0.0F)), PhysicsWorld::invalid_body);
    world->update(3.0F);
    EXPECT_LT(world->pending_time(), PhysicsWorld::fixed_timestep);
}

TEST_F(PhysicsWorldTest, DroppedSphereComesToRestOnTheFloor) {
    add_floor();
    const std::uint32_t sphere = add_ball(glm::vec3(0.0F, 5.0F, 0.0F));
    ASSERT_NE(sphere, PhysicsWorld::invalid_body);
    world->update(3.0F); // a load hitch first must not disturb the drop
    step(120);           // ~2 s at 60 fps

    const float y = position_of(sphere).y;
    EXPECT_LE(y, 4.5F) << "sphere did not fall";
    EXPECT_GE(y, 0.0F) << "sphere fell through the floor";
    EXPECT_NEAR(y, 0.5F, 0.25F) << "sphere did not come to rest on the floor";
}

// A dynamic sphere falling straight through a static sensor box reports both
// an enter and an exit event from the contact listener.
TEST_F(PhysicsWorldTest, SensorReportsEnterAndExitEvents) {
    // Static sensor box at y=3, half-extents (1,1,1) -> spans y in [2,4].
    PhysicsWorld::BodyParams trigger;
    trigger.shape = world->create_box(glm::vec3(1.0F, 1.0F, 1.0F));
    trigger.position = glm::vec3(0.0F, 3.0F, 0.0F);
    trigger.motion = Motion::static_body;
    trigger.layer = Layer::sensor;
    trigger.sensor = true;
    const std::uint32_t sensor = world->add_body(trigger);
    ASSERT_NE(sensor, PhysicsWorld::invalid_body);

    PhysicsWorld::BodyParams ball;
    ball.shape = world->create_sphere(0.5F);
    ball.position = glm::vec3(0.0F, 6.0F, 0.0F);
    ball.motion = Motion::dynamic;
    const std::uint32_t sphere = world->add_body(ball);
    ASSERT_NE(sphere, PhysicsWorld::invalid_body);

    bool entered = false;
    bool exited = false;
    for (int i = 0; i < 240; ++i) {
        world->update(k_dt);
        for (const PhysicsWorld::TriggerEvent& ev : world->trigger_events()) {
            if (ev.trigger == sensor && ev.other == sphere) {
                (ev.entered ? entered : exited) = true;
            }
        }
    }
    EXPECT_TRUE(entered) << "no enter event for the sensor";
    EXPECT_TRUE(exited) << "no exit event for the sensor";
}

} // namespace
