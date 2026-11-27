// SimClock — see engine/core/sim_clock.hpp.

#include "engine/core/sim_clock.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace engine::core {

float SimClock::advance(float real_dt) {
    float dt = 0.0F;
    if (run_count_ > 0) {
        StepRun& run = runs_[run_head_];
        dt = run.dt;
        if (--run.count == 0) {
            run_head_ = (run_head_ + 1) % max_step_runs;
            --run_count_;
        }
        --pending_steps_;
        ++steps_run_;
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
    run_head_ = 0;
    run_count_ = 0;
    pending_steps_ = 0;
}

std::expected<std::uint64_t, Error> SimClock::step(std::uint32_t frames, float dt) {
    dt = std::isfinite(dt) && dt > 0.0F ? std::min(dt, max_step_dt) : default_step_dt;
    if (frames > max_pending_steps - pending_steps_) {
        return std::unexpected(Error{
            "at most " + std::to_string(max_pending_steps) + " steps can be queued (" +
            std::to_string(pending_steps_) + " pending); wait for them to run or resume to drop "
            "them"});
    }
    StepRun* back = run_count_ > 0 ? &runs_[(run_head_ + run_count_ - 1) % max_step_runs] : nullptr;
    if (frames > 0 && (back == nullptr || back->dt != dt)) {
        if (run_count_ == max_step_runs) {
            return std::unexpected(Error{
                std::to_string(max_step_runs) + " step requests with different dt values are "
                "already queued; wait for them to run, repeat the dt of the last request, or "
                "resume to drop them"});
        }
        back = &runs_[(run_head_ + run_count_) % max_step_runs];
        *back = StepRun{0, dt};
        ++run_count_;
    }
    paused_ = true;
    if (back != nullptr) back->count += frames;
    pending_steps_ += frames;
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
