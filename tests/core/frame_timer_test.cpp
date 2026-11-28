// FrameTimer: real frame deltas, and no time leak after a minimised or
// skipped period (issue #61) — for real-dt consumers and, through SimClock,
// for the simulation.

#include <cstdint>

#include <gtest/gtest.h>

#include "engine/core/frame_timer.hpp"
#include "engine/core/sim_clock.hpp"

namespace {

using engine::core::FrameTimer;
using engine::core::SimClock;

constexpr std::uint64_t k_ms = 1'000'000; // nanoseconds

TEST(FrameTimer, MeasuresTheTimeBetweenTicks) {
    FrameTimer t;
    EXPECT_EQ(t.tick(5 * k_ms), 0.0F) << "no previous frame";
    EXPECT_FLOAT_EQ(t.tick(21 * k_ms), 0.016F);
    EXPECT_FLOAT_EQ(t.tick(54 * k_ms), 0.033F);
}

TEST(FrameTimer, ClockGoingBackwardsGivesZero) {
    FrameTimer t;
    (void)t.tick(100 * k_ms);
    EXPECT_EQ(t.tick(90 * k_ms), 0.0F);
    EXPECT_FLOAT_EQ(t.tick(116 * k_ms), 0.016F) << "keeps the latest reference";
}

// Setup before the first frame is not frame time.
TEST(FrameTimer, SkipBeforeTheFirstFrameStartsTheClock) {
    FrameTimer t;
    t.skip(1000 * k_ms);
    EXPECT_FLOAT_EQ(t.tick(1016 * k_ms), 0.016F);
}

// The frame loop's minimised path: every iteration skips; the first frame
// after restore measures from the last skip, not from before minimising.
TEST(FrameTimer, RestoreAfterMinimizeDoesNotLeakTheGap) {
    FrameTimer t;
    std::uint64_t now = 0;
    (void)t.tick(now);
    now += 16 * k_ms;
    EXPECT_FLOAT_EQ(t.tick(now), 0.016F);

    for (int i = 0; i < 3000; ++i) { // 30 s minimised, one 10 ms iteration each
        now += 10 * k_ms;
        t.skip(now);
    }
    now += 16 * k_ms;
    EXPECT_FLOAT_EQ(t.tick(now), 0.016F) << "the minimised 30 s leaked into the frame dt";
}

// The same gap through the whole real-time + simulation path: minimised
// iterations advance the simulation by nothing (advance_sim_unrendered), and
// restoring adds one ordinary frame, not 30 s.
TEST(FrameTimer, RestoreAfterMinimizeKeepsTheSimulationStill) {
    FrameTimer t;
    SimClock clock;
    std::uint64_t now = 0;
    (void)clock.advance(t.tick(now));
    for (int i = 0; i < 10; ++i) {
        now += 16 * k_ms;
        (void)clock.advance(t.tick(now));
    }
    const double before = clock.state().sim_time_s;
    EXPECT_NEAR(before, 0.16, 1e-5);

    for (int i = 0; i < 3000; ++i) {
        now += 10 * k_ms;
        t.skip(now);
        EXPECT_EQ(clock.advance(0.0F), 0.0F); // unrendered iteration
    }
    now += 16 * k_ms;
    const float sim_dt = clock.advance(t.tick(now));
    EXPECT_FLOAT_EQ(sim_dt, 0.016F);
    EXPECT_NEAR(clock.state().sim_time_s - before, 0.016, 1e-5);
}

} // namespace
