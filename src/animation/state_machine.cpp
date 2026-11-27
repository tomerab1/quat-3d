// Animation state machine with crossfade (Phase 5, Slice 5.6).

#include "engine/animation/state_machine.hpp"

#include <algorithm>
#include <cmath>

namespace engine::animation {

namespace {

void advance_time(float& time, const AnimState& state, float dt) {
    time += dt * state.speed;
    const float duration = state.clip != nullptr ? state.clip->duration : 0.0F;
    if (duration <= 0.0F) {
        return;
    }
    if (state.looping) {
        time = std::fmod(time, duration);
        if (time < 0.0F) time += duration;
    } else {
        time = std::clamp(time, 0.0F, duration);
    }
}

[[nodiscard]] std::vector<JointPose> state_pose(const SkeletonAsset& skeleton,
                                                const AnimState& state, float time) {
    if (state.clip != nullptr) {
        return sample_local_pose(skeleton, *state.clip, time);
    }
    return bind_pose(skeleton);
}

} // namespace

int AnimStateMachine::add_state(AnimState state) {
    states_.push_back(std::move(state));
    return static_cast<int>(states_.size()) - 1;
}

void AnimStateMachine::add_transition(AnimTransition trans) {
    transitions_.push_back(std::move(trans));
}

void AnimStateMachine::set_parameter(const std::string& name, float value) {
    parameters_[name] = value;
}

void AnimStateMachine::set_current(int state) {
    current_ = state;
    current_time_ = 0.0F;
    target_ = -1;
    transition_elapsed_ = 0.0F;
    transition_duration_ = 0.0F;
}

bool AnimStateMachine::condition_met(const AnimCondition& c) const {
    const auto it = parameters_.find(c.parameter);
    const float value = it != parameters_.end() ? it->second : 0.0F;
    return c.op == AnimCondition::Op::greater ? value > c.threshold : value < c.threshold;
}

std::vector<glm::mat4> AnimStateMachine::update(const SkeletonAsset& skeleton, float dt) {
    if (current_ < 0 && !states_.empty()) {
        current_ = 0;
    }
    if (current_ < 0) {
        return bind_skinning_matrices(skeleton);
    }

    advance_time(current_time_, states_[static_cast<std::size_t>(current_)], dt);
    if (target_ >= 0) {
        advance_time(target_time_, states_[static_cast<std::size_t>(target_)], dt);
    }

    if (target_ >= 0) {
        // Mid-crossfade: complete it when the duration elapses.
        transition_elapsed_ += dt;
        if (transition_elapsed_ >= transition_duration_) {
            current_ = target_;
            current_time_ = target_time_;
            target_ = -1;
        }
    } else {
        // Look for a transition whose condition holds.
        for (const AnimTransition& t : transitions_) {
            if ((t.from == -1 || t.from == current_) && t.to != current_ &&
                condition_met(t.condition)) {
                target_ = t.to;
                target_time_ = 0.0F;
                transition_elapsed_ = 0.0F;
                transition_duration_ = std::max(t.duration, 1e-4F);
                break;
            }
        }
    }

    std::vector<JointPose> pose =
        state_pose(skeleton, states_[static_cast<std::size_t>(current_)], current_time_);
    if (target_ >= 0) {
        const std::vector<JointPose> target_pose =
            state_pose(skeleton, states_[static_cast<std::size_t>(target_)], target_time_);
        const float w = std::clamp(transition_elapsed_ / transition_duration_, 0.0F, 1.0F);
        pose = blend_poses(pose, target_pose, w);
    }
    return pose_skinning_matrices(skeleton, pose);
}

} // namespace engine::animation
