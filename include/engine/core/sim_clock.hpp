#pragma once

// SimClock — the simulation's notion of time (ROADMAP A5): pause, single-step
// and time scale for debugging, independent of the real frame time that
// drives the camera, UI, audio, networking and rendering.
//
// The frame loop calls advance(real_dt) once per frame and feeds the returned
// dt to the simulation systems (physics, characters, AI, navigation,
// animation). Running, that is real_dt * time_scale; paused, it is 0 — except
// for frames consumed by step(), which each advance by exactly the dt that
// step() was given (unscaled, so a stepped trace is reproducible regardless
// of frame rate). Steps queued by successive step() calls run in order, each
// with its own dt.

#include <array>
#include <cstdint>
#include <expected>

#include "engine/core/error.hpp"

namespace engine::core {

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
    static constexpr float default_step_dt = 1.0F / 60.0F;
    static constexpr float max_step_dt = 0.1F;
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
