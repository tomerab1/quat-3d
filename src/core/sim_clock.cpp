// SimClock — see engine/core/sim_clock.hpp.

#include "engine/core/sim_clock.hpp"

#include <algorithm>
#include <cmath>

namespace engine::core {

SimSlices slice_sim_dt(float dt, float max_slice) {
    if (!std::isfinite(dt) || dt <= 0.0F) return {};
    if (!std::isfinite(max_slice) || max_slice <= 0.0F || dt <= max_slice) return {1, dt};
    const float needed = std::ceil(dt / max_slice);
    if (!(needed < static_cast<float>(SimSlices::max_count))) {
        return {SimSlices::max_count, dt / static_cast<float>(SimSlices::max_count)};
    }
    auto count = static_cast<std::uint32_t>(needed);
    // Rounding must never leave a slice above the budget.
    while (dt / static_cast<float>(count) > max_slice) ++count;
    return {count, dt / static_cast<float>(count)};
}

float SimClock::advance(float real_dt) {
    float dt = 0.0F;
    if (pending_steps_ > 0) {
        --pending_steps_;
        ++steps_run_;
        dt = step_dt_;
    } else if (!paused_ && std::isfinite(real_dt) && real_dt > 0.0F) {
        dt = std::min(real_dt, max_frame_dt) * time_scale_;
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
