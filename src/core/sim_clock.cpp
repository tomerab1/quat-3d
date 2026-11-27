// SimClock — see engine/core/sim_clock.hpp.

#include "engine/core/sim_clock.hpp"

#include <algorithm>
#include <cmath>

namespace engine::core {

float SimClock::advance(float real_dt) {
    float dt = 0.0F;
    if (pending_steps_ > 0) {
        --pending_steps_;
        ++steps_run_;
        dt = step_dt_;
    } else if (!paused_ && std::isfinite(real_dt) && real_dt > 0.0F) {
        dt = real_dt * time_scale_;
    }
    if (dt > 0.0F) {
        ++sim_frames_;
        sim_time_s_ += static_cast<double>(dt);
    }
    return dt;
}

void SimClock::resume() {
    paused_ = false;
    pending_steps_ = 0;
}

std::uint64_t SimClock::step(std::uint32_t frames, float dt) {
    paused_ = true;
    if (std::isfinite(dt) && dt > 0.0F) step_dt_ = std::min(dt, max_step_dt);
    pending_steps_ = std::min(max_pending_steps, pending_steps_ + std::min(frames, max_pending_steps));
    return steps_run_ + pending_steps_;
}

void SimClock::set_time_scale(float scale) {
    if (!std::isfinite(scale)) return;
    time_scale_ = std::clamp(scale, min_time_scale, max_time_scale);
}

SimClockState SimClock::state() const {
    return SimClockState{paused_, time_scale_, pending_steps_, sim_frames_, sim_time_s_, steps_run_};
}

} // namespace engine::core
