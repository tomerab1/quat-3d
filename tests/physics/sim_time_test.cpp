// Audit D2: the debug simulation clock (time scale, single steps) must
// advance rigid bodies by the same simulated time as every other simulation
// system. The frame loop (WorldRuntime::simulate) feeds each frame's clock dt
// to the systems in core::slice_sim_dt slices of at most
// PhysicsWorld::max_update_dt; these tests drive a PhysicsWorld the same way
// and compare it against the clock's simulated time and against a character
// controller (a system that integrates whatever dt it is given).

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/core/sim_clock.hpp"
#include "engine/physics/physics_world.hpp"
#include "support/expect_ok.hpp"

namespace {

using engine::core::SimClock;
using engine::physics::Motion;
using engine::physics::PhysicsWorld;

constexpr float     k_real_dt = 1.0F / 60.0F;
constexpr float     k_walk_speed = 2.0F; // m/s, the character's constant velocity
const glm::vec3     k_ball_start(0.0F, 1000.0F, 0.0F);
const glm::vec3     k_character_start(50.0F, 0.0F, 0.0F);

// A world with a free-falling ball (no floor) and, optionally, a character
// walking along +X far away from it.
struct World {
    std::optional<PhysicsWorld> physics;
    std::uint32_t               ball = PhysicsWorld::invalid_body;
    std::uint32_t               character = PhysicsWorld::invalid_body;

    explicit World(bool with_character) {
        auto w = PhysicsWorld::create();
        EXPECT_OK(w);
        if (!w) return;
        physics.emplace(std::move(*w));
        PhysicsWorld::BodyParams params;
        params.shape = physics->create_sphere(0.5F);
        params.position = k_ball_start;
        params.motion = Motion::dynamic;
        ball = physics->add_body(params);
        if (with_character) {
            character = physics->create_character(1.0F, 0.4F, k_character_start);
            physics->character_set_velocity(character, glm::vec3(k_walk_speed, 0.0F, 0.0F));
        }
    }

    [[nodiscard]] glm::vec3 ball_position() const {
        glm::vec3 p(0.0F);
        glm::quat q(1.0F, 0.0F, 0.0F, 0.0F);
        physics->body_transform(ball, p, q);
        return p;
    }

    // One frame of the clock, run like WorldRuntime::simulate: every system
    // gets each slice.
    void simulate_frame(float sim_dt) {
        const engine::core::SimSlices slices =
            engine::core::slice_sim_dt(sim_dt, PhysicsWorld::max_update_dt);
        for (std::uint32_t i = 0; i < slices.count; ++i) {
            physics->character_update(character, slices.dt);
            physics->update(slices.dt);
        }
    }
};

// The ball's position after exactly `ticks` fixed physics steps.
glm::vec3 reference_ball_after(int ticks) {
    World ref(false);
    for (int i = 0; i < ticks; ++i) ref.physics->update(PhysicsWorld::fixed_timestep);
    return ref.ball_position();
}

// Physics has simulated `sim_time` (to within float rounding): its whole
// ticks plus the carried remainder add up to it, and the ball sits where
// exactly that many ticks put it.
void expect_physics_simulated(const World& w, double sim_time) {
    const double simulated_ticks =
        (sim_time - static_cast<double>(w.physics->pending_time())) /
        static_cast<double>(PhysicsWorld::fixed_timestep);
    const int ticks = static_cast<int>(std::lround(simulated_ticks));
    EXPECT_NEAR(simulated_ticks, ticks, 1e-2) << "physics lost or gained time";
    EXPECT_LT(w.physics->pending_time(), PhysicsWorld::fixed_timestep * 1.01F);
    const glm::vec3 expected = reference_ball_after(ticks);
    const glm::vec3 actual = w.ball_position();
    EXPECT_NEAR(actual.y, expected.y, 1e-3F) << "after " << ticks << " ticks";
    EXPECT_LT(actual.y, k_ball_start.y) << "the ball must have fallen";
}

TEST(PhysicsSimTime, TimeScaleTenAdvancesBodiesAndCharactersEqually) {
    World w(true);
    ASSERT_TRUE(w.physics);
    SimClock clock;
    clock.set_time_scale(SimClock::max_time_scale);
    for (int frame = 0; frame < 60; ++frame) {
        const float sim_dt = clock.advance(k_real_dt);
        ASSERT_GT(sim_dt, PhysicsWorld::max_update_dt) << "the case audit D2 describes";
        w.simulate_frame(sim_dt);
    }
    const double sim_time = clock.state().sim_time_s;
    EXPECT_NEAR(sim_time, 10.0, 1e-3); // one real second at 10x

    expect_physics_simulated(w, sim_time);
    const float walked = w.physics->character_position(w.character).x - k_character_start.x;
    EXPECT_NEAR(walked, k_walk_speed * static_cast<float>(sim_time), 1e-2F);
}

TEST(PhysicsSimTime, LargestStepAdvancesBodiesByExactlyTheStep) {
    World w(true);
    ASSERT_TRUE(w.physics);
    SimClock clock;
    (void)clock.step(3, 0.1F); // above the largest step: clamped to it
    for (int frame = 0; frame < 3; ++frame) {
        const float sim_dt = clock.advance(k_real_dt);
        EXPECT_FLOAT_EQ(sim_dt, SimClock::max_step_dt);
        w.simulate_frame(sim_dt);
    }
    EXPECT_EQ(clock.advance(k_real_dt), 0.0F) << "paused after the steps";

    // Each step is exactly one physics update's budget: 8 ticks.
    EXPECT_EQ(PhysicsWorld::max_update_dt, SimClock::max_step_dt);
    expect_physics_simulated(w, clock.state().sim_time_s);
    EXPECT_NEAR(w.ball_position().y, reference_ball_after(24).y, 1e-3F);
    const float walked = w.physics->character_position(w.character).x - k_character_start.x;
    EXPECT_NEAR(walked, k_walk_speed * 3.0F * SimClock::max_step_dt, 1e-3F);
}

// A hitch drops the same real time for every system: physics no longer runs
// behind the clock that animation and navigation follow.
TEST(PhysicsSimTime, HitchIsDroppedForEverySystemAlike) {
    World w(true);
    ASSERT_TRUE(w.physics);
    SimClock clock;
    w.simulate_frame(clock.advance(0.75F)); // a blocking load
    for (int frame = 0; frame < 30; ++frame) w.simulate_frame(clock.advance(k_real_dt));
    expect_physics_simulated(w, clock.state().sim_time_s);
    const float walked = w.physics->character_position(w.character).x - k_character_start.x;
    EXPECT_NEAR(walked, k_walk_speed * static_cast<float>(clock.state().sim_time_s), 1e-3F);
}

} // namespace
