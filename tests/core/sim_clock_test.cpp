// SimClock: running / paused / scaled time and fixed-size single steps.

#include <limits>

#include <gtest/gtest.h>

#include "engine/core/sim_clock.hpp"

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
    EXPECT_EQ(target, 3U);
    EXPECT_TRUE(c.state().paused);
    EXPECT_EQ(c.state().pending_steps, 3U);
    for (int i = 0; i < 3; ++i) EXPECT_FLOAT_EQ(c.advance(0.5F), 0.01F);
    EXPECT_EQ(c.advance(0.5F), 0.0F) << "paused after the steps";
    EXPECT_EQ(c.state().steps_run, 3U);
    EXPECT_EQ(c.state().sim_frames, 3U);
    EXPECT_NEAR(c.state().sim_time_s, 0.03, 1e-6);

    // Further steps queue after what already ran.
    EXPECT_EQ(c.step(2), 5U);
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

} // namespace
