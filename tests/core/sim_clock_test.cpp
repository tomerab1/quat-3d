// SimClock: running / paused / scaled time and fixed-size single steps.

#include <cmath>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "engine/core/sim_clock.hpp"
#include "support/expect_ok.hpp"

namespace {

using engine::core::SimClock;

TEST(SimClock, RunsInRealTimeByDefault) {
    SimClock c;
    EXPECT_FLOAT_EQ(c.advance(0.016F), 0.016F);
    EXPECT_EQ(c.state().sim_frames, 1U);
    EXPECT_NEAR(c.state().sim_time_s, 0.016, 1e-6);
}

TEST(SimClock, PauseFreezesAndResumeContinues) {
    SimClock c;
    c.pause();
    EXPECT_EQ(c.advance(0.016F), 0.0F);
    EXPECT_EQ(c.advance(0.016F), 0.0F);
    EXPECT_EQ(c.state().sim_frames, 0U);
    EXPECT_TRUE(c.state().paused);
    c.resume();
    EXPECT_FLOAT_EQ(c.advance(0.02F), 0.02F);
}

TEST(SimClock, TimeScaleIsClamped) {
    SimClock c;
    c.set_time_scale(0.5F);
    EXPECT_FLOAT_EQ(c.advance(0.02F), 0.01F);
    c.set_time_scale(1000.0F);
    EXPECT_FLOAT_EQ(c.state().time_scale, SimClock::max_time_scale);
    c.set_time_scale(0.0F);
    EXPECT_FLOAT_EQ(c.state().time_scale, SimClock::min_time_scale);
}

TEST(SimClock, StepsRunOnePerFrameWithAFixedDt) {
    SimClock c;
    c.set_time_scale(3.0F); // steps are unscaled
    const auto target = c.step(3, 0.01F);
    ASSERT_OK(target);
    EXPECT_EQ(*target, 3U);
    EXPECT_TRUE(c.state().paused);
    EXPECT_EQ(c.state().pending_steps, 3U);
    for (int i = 0; i < 3; ++i) EXPECT_FLOAT_EQ(c.advance(0.5F), 0.01F);
    EXPECT_EQ(c.advance(0.5F), 0.0F) << "paused after the steps";
    EXPECT_EQ(c.state().steps_run, 3U);
    EXPECT_EQ(c.state().sim_frames, 3U);
    EXPECT_NEAR(c.state().sim_time_s, 0.03, 1e-6);

    // Further steps queue after what already ran.
    EXPECT_EQ(c.step(2).value_or(0), 5U);
    EXPECT_FLOAT_EQ(c.advance(0.5F), SimClock::default_step_dt);
}

TEST(SimClock, ResumeDropsPendingSteps) {
    SimClock c;
    (void)c.step(10);
    (void)c.advance(0.016F);
    c.resume();
    EXPECT_EQ(c.state().pending_steps, 0U);
    EXPECT_EQ(c.state().steps_run, 1U);
    EXPECT_FLOAT_EQ(c.advance(0.016F), 0.016F);
}

TEST(SimClock, IgnoresBadFrameTimes) {
    SimClock c;
    EXPECT_EQ(c.advance(-1.0F), 0.0F);
    EXPECT_EQ(c.advance(std::numeric_limits<float>::quiet_NaN()), 0.0F);
    EXPECT_EQ(c.state().sim_frames, 0U);
    (void)c.step(1, 5.0F); // clamped
    EXPECT_FLOAT_EQ(c.advance(0.0F), SimClock::max_step_dt);
}

// Audit D2: real time beyond max_frame_dt is a hitch, dropped before scaling,
// so every simulation system sees the same bounded dt.
TEST(SimClock, DropsHitchBacklogBeforeScaling) {
    SimClock c;
    EXPECT_FLOAT_EQ(c.advance(0.5F), SimClock::max_frame_dt);
    c.set_time_scale(10.0F);
    EXPECT_FLOAT_EQ(c.advance(0.5F), SimClock::max_frame_dt * 10.0F);
    EXPECT_FLOAT_EQ(c.advance(1.0F / 60.0F), 10.0F / 60.0F); // below the cap: unchanged
}

TEST(SimClock, SlicesCoverTheDtWithinTheBudget) {
    using engine::core::slice_sim_dt;
    constexpr float budget = 8.0F / 120.0F;

    const auto one = slice_sim_dt(0.016F, budget);
    EXPECT_EQ(one.count, 1U);
    EXPECT_FLOAT_EQ(one.dt, 0.016F);
    const auto exact = slice_sim_dt(budget, budget);
    EXPECT_EQ(exact.count, 1U);

    for (const float dt : {0.0667F, 0.1F, 10.0F / 60.0F, 0.5F, 2.0F / 3.0F, 1.0F}) {
        const auto s = slice_sim_dt(dt, budget);
        EXPECT_EQ(s.count, static_cast<std::uint32_t>(std::ceil(dt / budget))) << dt;
        EXPECT_LE(s.dt, budget) << dt;
        EXPECT_NEAR(s.dt * static_cast<float>(s.count), dt, 1e-6F) << dt;
    }

    EXPECT_EQ(slice_sim_dt(0.0F, budget).count, 0U);
    EXPECT_EQ(slice_sim_dt(-1.0F, budget).count, 0U);
    EXPECT_EQ(slice_sim_dt(std::numeric_limits<float>::infinity(), budget).count, 0U);
    EXPECT_EQ(slice_sim_dt(0.5F, 0.0F).count, 1U); // no budget: one slice
    const auto many = slice_sim_dt(1.0e9F, 1.0e-6F);
    EXPECT_EQ(many.count, engine::core::SimSlices::max_count);
}

// A second step request must not change the dt of steps already queued.
// (The dt values sit below SimClock::max_step_dt so no request is clamped;
// QueuedStepsKeepTheirDtAtTheCap covers the cap.)
TEST(SimClock, QueuedStepsKeepTheirOwnDt) {
    SimClock c;
    ASSERT_OK(c.step(3, 0.05F));
    EXPECT_FLOAT_EQ(c.advance(0.5F), 0.05F);
    const auto target = c.step(2, 0.02F);
    ASSERT_OK(target);
    EXPECT_EQ(*target, 5U);
    ASSERT_OK(c.step(1, 0.02F)); // same dt: joins the last run
    EXPECT_FLOAT_EQ(c.advance(0.5F), 0.05F);
    EXPECT_FLOAT_EQ(c.advance(0.5F), 0.05F);
    for (int i = 0; i < 3; ++i) EXPECT_FLOAT_EQ(c.advance(0.5F), 0.02F);
    EXPECT_EQ(c.advance(0.5F), 0.0F);
    EXPECT_EQ(c.state().steps_run, 6U);
    EXPECT_NEAR(c.state().sim_time_s, 0.21, 1e-6);
}

// At the step cap (one physics update, 1/15 s): a step of exactly
// max_step_dt keeps it, a larger request is clamped to it on entry — so it
// joins a queued run of max_step_dt — and later requests with another dt
// still leave both runs' dt alone.
TEST(SimClock, QueuedStepsKeepTheirDtAtTheCap) {
    SimClock c;
    ASSERT_OK(c.step(2, SimClock::max_step_dt));
    ASSERT_OK(c.step(1, 0.5F)); // clamped: same dt as the run before, joins it
    ASSERT_OK(c.step(2, 0.01F));
    ASSERT_OK(c.step(1, 1.0F)); // clamped again: a new run after the 0.01 run
    for (int i = 0; i < 3; ++i) EXPECT_FLOAT_EQ(c.advance(1.0F), SimClock::max_step_dt) << i;
    for (int i = 0; i < 2; ++i) EXPECT_FLOAT_EQ(c.advance(1.0F), 0.01F) << i;
    EXPECT_FLOAT_EQ(c.advance(1.0F), SimClock::max_step_dt);
    EXPECT_EQ(c.advance(1.0F), 0.0F);
    EXPECT_EQ(c.state().steps_run, 6U);
}

TEST(SimClock, RejectsStepsBeyondItsQueueLimits) {
    SimClock c;
    // Alternating dt values: one run each, up to max_step_runs.
    for (std::uint32_t i = 0; i < SimClock::max_step_runs; ++i) {
        ASSERT_OK(c.step(1, i % 2 == 0 ? 0.01F : 0.02F)) << i;
    }
    const float last = SimClock::max_step_runs % 2 == 0 ? 0.02F : 0.01F;
    ASSERT_ERR(c.step(1, last == 0.01F ? 0.02F : 0.01F));
    EXPECT_EQ(c.state().pending_steps, SimClock::max_step_runs) << "a refused step queues nothing";
    ASSERT_OK(c.step(4, last)) << "the last run's dt still joins it";
    EXPECT_EQ(c.state().pending_steps, SimClock::max_step_runs + 4);

    // Running a whole run frees its slot.
    EXPECT_FLOAT_EQ(c.advance(0.0F), 0.01F);
    ASSERT_OK(c.step(1, 0.05F));

    // The total is capped too.
    c.resume();
    ASSERT_OK(c.step(SimClock::max_pending_steps));
    ASSERT_ERR(c.step(1));
    EXPECT_EQ(c.state().pending_steps, SimClock::max_pending_steps);
}

// A frame that draws nothing (minimised window) passes real_dt 0: only a
// queued step advances the simulation.
TEST(SimClock, ZeroRealDtRunsOnlyQueuedSteps) {
    SimClock c;
    EXPECT_EQ(c.advance(0.0F), 0.0F);
    ASSERT_OK(c.step(1, 0.03F));
    EXPECT_FLOAT_EQ(c.advance(0.0F), 0.03F);
    EXPECT_EQ(c.state().steps_run, 1U);
    EXPECT_EQ(c.advance(0.0F), 0.0F);
}

} // namespace
