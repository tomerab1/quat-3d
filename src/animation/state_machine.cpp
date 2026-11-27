// Animation state machine with inertialized or crossfaded transitions (5.6, G8).

#include "engine/animation/state_machine.hpp"

#include <algorithm>
#include <cmath>

namespace engine::animation {

namespace {

// Wrap (looping) or clamp `time` into the state's clip range.
[[nodiscard]] float wrap_time(const AnimState& state, float time) {
    const float duration = state.clip != nullptr ? state.clip->duration : 0.0F;
    if (duration <= 0.0F) return time;
    if (state.looping) {
        time = std::fmod(time, duration);
        return time < 0.0F ? time + duration : time;
    }
    return std::clamp(time, 0.0F, duration);
}

void advance_time(float& time, const AnimState& state, float dt) {
    time = wrap_time(state, time + dt * state.speed);
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
    inertializer_.reset();
}

bool AnimStateMachine::condition_met(const AnimCondition& c) const {
    const auto it = parameters_.find(c.parameter);
    const float value = it != parameters_.end() ? it->second : 0.0F;
    return c.op == AnimCondition::Op::greater ? value > c.threshold : value < c.threshold;
}

std::vector<JointPose> AnimStateMachine::state_pose(const SkeletonAsset& skeleton, int state,
                                                    float time) {
    const AnimState& st = states_[static_cast<std::size_t>(state)];
    if (st.clip == nullptr) return bind_pose(skeleton);
    ++evaluations_;
    return sample_local_pose(skeleton, *st.clip, time);
}

std::vector<JointPose> AnimStateMachine::begin_transition(const SkeletonAsset& skeleton,
                                                          float dt) {
    for (const AnimTransition& t : transitions_) {
        if ((t.from != -1 && t.from != current_) || t.to == current_ || t.to < 0 ||
            static_cast<std::size_t>(t.to) >= states_.size() || !condition_met(t.condition)) {
            continue;
        }
        if (t.blend == BlendMode::crossfade) {
            target_ = t.to;
            target_time_ = 0.0F;
            transition_elapsed_ = 0.0F;
            transition_duration_ = std::max(t.duration, 1e-4F);
            return {};
        }
        // Inertialize: switch now; the outgoing pose lives on as a decaying
        // offset measured at the previous frame.
        const AnimState& to = states_[static_cast<std::size_t>(t.to)];
        std::vector<JointPose> pose = state_pose(skeleton, t.to, 0.0F);
        const std::size_t joints = skeleton.joints.size();
        if (last_pose_.size() == joints && prev_pose_.size() == joints && t.duration > 0.0F) {
            const std::vector<JointPose> before =
                state_pose(skeleton, t.to, wrap_time(to, -dt * to.speed));
            inertializer_.begin(last_pose_, prev_pose_, last_dt_, before, pose, dt, t.duration);
            inertializer_.advance(dt);
        } else {
            inertializer_.reset();
        }
        current_ = t.to;
        current_time_ = 0.0F;
        return pose;
    }
    return {};
}

std::vector<glm::mat4> AnimStateMachine::update(const SkeletonAsset& skeleton, float dt) {
    evaluations_ = 0;
    if (current_ < 0 && !states_.empty()) {
        current_ = 0;
    }
    if (current_ < 0) {
        return bind_skinning_matrices(skeleton);
    }

    advance_time(current_time_, states_[static_cast<std::size_t>(current_)], dt);
    if (inertializer_.active()) inertializer_.advance(dt);

    std::vector<JointPose> pose;
    if (target_ >= 0) {
        // Mid-crossfade: complete it when the duration elapses.
        advance_time(target_time_, states_[static_cast<std::size_t>(target_)], dt);
        transition_elapsed_ += dt;
        if (transition_elapsed_ >= transition_duration_) {
            current_ = target_;
            current_time_ = target_time_;
            target_ = -1;
        }
    } else {
        pose = begin_transition(skeleton, dt);
    }

    if (pose.empty()) pose = state_pose(skeleton, current_, current_time_);
    if (target_ >= 0) {
        const std::vector<JointPose> target_pose = state_pose(skeleton, target_, target_time_);
        const float w = std::clamp(transition_elapsed_ / transition_duration_, 0.0F, 1.0F);
        pose = blend_poses(pose, target_pose, w);
    }
    inertializer_.apply(pose);

    prev_pose_ = std::move(last_pose_);
    last_pose_ = pose;
    last_dt_ = dt;
    return pose_skinning_matrices(skeleton, pose);
}

} // namespace engine::animation
