#pragma once

// SimClock — the simulation's notion of time (ROADMAP A5): pause, single-step
// and time scale for debugging, independent of the real frame time that
// drives the camera, UI, audio, networking and rendering.
//
// The frame loop calls advance(real_dt) once per frame and feeds the returned
// dt to the simulation systems (physics, characters, AI, navigation,
// animation). Running, that is real_dt * time_scale; paused, it is 0 — except
// for frames consumed by step(), which each advance by exactly step_dt
// (unscaled, so a stepped trace is reproducible regardless of frame rate).

#include <cstdint>

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

    // Simulation dt for this frame (see the header comment).
    [[nodiscard]] float advance(float real_dt);

    void pause() { paused_ = true; }
    // Resumes real-time simulation; pending single steps are dropped.
    void resume();
    // Pauses (if running) and queues `frames` single steps of `dt` seconds
    // each (clamped to (0, max_step_dt]). Returns the steps_run value at
    // which the last queued step will have run.
    std::uint64_t step(std::uint32_t frames, float dt = default_step_dt);
    // Clamped to [min_time_scale, max_time_scale].
    void set_time_scale(float scale);

    [[nodiscard]] SimClockState state() const;

private:
    bool          paused_ = false;
    float         time_scale_ = 1.0F;
    float         step_dt_ = default_step_dt;
    std::uint32_t pending_steps_ = 0;
    std::uint64_t sim_frames_ = 0;
    double        sim_time_s_ = 0.0;
    std::uint64_t steps_run_ = 0;
};

} // namespace engine::core
