#pragma once

// SimClock — the simulation's notion of time (ROADMAP A5): pause, single-step
// and time scale for debugging, independent of the real frame time that
// drives the camera, UI, audio, networking and rendering.
//
// The frame loop calls advance(real_dt) once per frame and feeds the returned
// dt to the simulation systems (physics, characters, AI, navigation,
// animation). Running, that is min(real_dt, max_frame_dt) * time_scale;
// paused, it is 0 — except for frames consumed by step(), which each advance
// by exactly the dt that step() was given (unscaled, so a stepped trace is
// reproducible regardless of frame rate). Steps queued by successive step()
// calls run in order, each with its own dt.
//
// Every simulation system advances by the same dt (audit D2). Real time
// beyond max_frame_dt is a hitch (a blocking load, a breakpoint, a slow
// machine) and is dropped for all of them alike: the world runs slower than
// real time instead of taking ever larger steps. max_frame_dt equals the
// physics catch-up budget per update (PhysicsWorld::max_update_dt, 8 x 1/120
// s), so a frame at time scale 1 and every single step fit in one physics
// update. A scaled dt can exceed it (up to max_time_scale x max_frame_dt):
// the frame loop then runs the simulation systems in slice_sim_dt() slices of
// at most that budget, so physics simulates the requested time exactly
// rather than dropping it as a hitch.

#include <array>
#include <cstdint>
#include <expected>

#include "engine/core/error.hpp"

namespace engine::core {

// A frame's simulation dt split into `count` equal slices of `dt` seconds.
struct SimSlices {
    // Upper bound on `count`: past it the slices grow beyond `max_slice`
    // (no frame-loop dt comes near it: max_time_scale x max_frame_dt is 10
    // physics budgets).
    static constexpr std::uint32_t max_count = 1U << 16U;

    std::uint32_t count = 0;
    float         dt = 0.0F;
};

// The fewest equal slices of at most `max_slice` seconds covering `dt`
// (count 0 for a zero, negative or non-finite dt; a non-positive or
// non-finite `max_slice` gives one slice; at most SimSlices::max_count).
[[nodiscard]] SimSlices slice_sim_dt(float dt, float max_slice);

struct SimClockState {
    bool          paused = false;
    float         time_scale = 1.0F;
    std::uint32_t pending_steps = 0; // single steps queued, not yet run
    std::uint64_t sim_frames = 0;    // frames that advanced the simulation (dt > 0)
    double        sim_time_s = 0.0;  // simulated seconds so far
    std::uint64_t steps_run = 0;     // single steps executed so far
};

class SimClock {
public:
    static constexpr float min_time_scale = 0.01F;
    static constexpr float max_time_scale = 10.0F;
    // Real frame time the simulation follows at most per frame (see above).
    static constexpr float max_frame_dt = 8.0F / 120.0F;
    static constexpr float default_step_dt = 1.0F / 60.0F;
    // The largest single step: what one physics update simulates exactly.
    static constexpr float max_step_dt = max_frame_dt;
    static constexpr std::uint32_t max_pending_steps = 100'000;
    // Queued runs of steps with differing dt (consecutive requests with the
    // same dt share one run).
    static constexpr std::uint32_t max_step_runs = 64;

    // Simulation dt for this frame (see the header comment).
    [[nodiscard]] float advance(float real_dt);

    void pause() { paused_ = true; }
    // Resumes real-time simulation; pending single steps are dropped.
    void resume();
    // Pauses (if running) and queues `frames` single steps of `dt` seconds
    // each (clamped to (0, max_step_dt]; a non-finite or non-positive dt
    // means default_step_dt) after any already queued. Returns the steps_run
    // value at which the last queued step will have run, or an error (and
    // queues nothing) when that would exceed max_pending_steps or
    // max_step_runs.
    [[nodiscard]] std::expected<std::uint64_t, Error> step(std::uint32_t frames,
                                                           float dt = default_step_dt);
    // Clamped to [min_time_scale, max_time_scale].
    void set_time_scale(float scale);

    [[nodiscard]] SimClockState state() const;

private:
    struct StepRun {
        std::uint32_t count = 0;
        float         dt = default_step_dt;
    };

    bool          paused_ = false;
    float         time_scale_ = 1.0F;
    // FIFO ring of queued runs: `run_count_` entries starting at `run_head_`.
    std::array<StepRun, max_step_runs> runs_{};
    std::uint32_t run_head_ = 0;
    std::uint32_t run_count_ = 0;
    std::uint32_t pending_steps_ = 0; // sum of the queued runs' counts
    std::uint64_t sim_frames_ = 0;
    double        sim_time_s_ = 0.0;
    std::uint64_t steps_run_ = 0;
};

} // namespace engine::core
