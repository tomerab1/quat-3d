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

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
namespace {

[[nodiscard]] SkeletonAsset two_joint_skeleton() {
    SkeletonAsset skel;
    skel.joints.resize(2);
    skel.joints[0].name = "root";
    skel.joints[0].parent = -1;
    skel.joints[0].node = 0;
    skel.joints[1].name = "child";
    skel.joints[1].parent = 0;
    skel.joints[1].node = 1;
    skel.joints[1].translation = {2.0F, 0.0F, 0.0F};
    const std::vector<glm::mat4> bw = compute_bind_world(skel);
    for (std::size_t i = 0; i < skel.joints.size(); ++i) {
        skel.joints[i].inverse_bind = glm::inverse(bw[i]);
    }
    return skel;
}

[[nodiscard]] AnimClipAsset child_translation_clip(glm::vec3 t) {
    AnimClipAsset clip;
    clip.duration = 1.0F;
    AnimSampler s;
    s.input = {0.0F, 1.0F};
    s.output = {glm::vec4(t, 0.0F), glm::vec4(t, 0.0F)};
    clip.samplers = {s};
    clip.channels = {AnimChannel{1, AnimPath::translation, 0}};
    return clip;
}

} // namespace

std::expected<void, core::Error> run_state_machine_self_test() {
    const SkeletonAsset skel = two_joint_skeleton();
    const AnimClipAsset idle = child_translation_clip({2.0F, 0.0F, 0.0F}); // = bind
    const AnimClipAsset walk = child_translation_clip({5.0F, 0.0F, 0.0F});

    AnimStateMachine sm;
    const int idle_state = sm.add_state({"idle", &idle, 1.0F, true});
    const int walk_state = sm.add_state({"walk", &walk, 1.0F, true});
    sm.add_transition({idle_state, walk_state, {"speed", AnimCondition::Op::greater, 0.5F}, 1.0F});
    sm.set_current(idle_state);

    // Idle, speed below threshold: stays idle, child at bind -> skinning identity.
    std::vector<glm::mat4> m = sm.update(skel, 0.0F);
    if (sm.current_state() != idle_state || sm.transitioning()) {
        return std::unexpected(core::Error{"state machine self-test: should still be idle"});
    }
    if (glm::distance(m[1][3], glm::vec4(0, 0, 0, 1)) > 1e-4F) {
        return std::unexpected(core::Error{"state machine self-test: idle pose not bind"});
    }

    // Raise speed: a transition to walk begins (w = 0, still the idle pose).
    sm.set_parameter("speed", 1.0F);
    m = sm.update(skel, 0.0F);
    if (!sm.transitioning()) {
        return std::unexpected(core::Error{"state machine self-test: transition did not start"});
    }

    // Halfway through the 1s crossfade: child blends idle(2) -> walk(5) at 0.5 =
    // 3.5, so skinning displaces a bind vertex by (1.5, 0, 0).
    m = sm.update(skel, 0.5F);
    if (glm::distance(glm::vec3(m[1][3]), glm::vec3(1.5F, 0.0F, 0.0F)) > 1e-3F) {
        return std::unexpected(core::Error{"state machine self-test: mid-crossfade pose wrong"});
    }

    // Completing the crossfade lands on walk: child at (5,0,0), displacement (3,0,0).
    m = sm.update(skel, 0.5F);
    if (sm.transitioning() || sm.current_state() != walk_state) {
        return std::unexpected(core::Error{"state machine self-test: transition did not complete"});
    }
    if (glm::distance(glm::vec3(m[1][3]), glm::vec3(3.0F, 0.0F, 0.0F)) > 1e-3F) {
        return std::unexpected(core::Error{"state machine self-test: final walk pose wrong"});
    }
    return {};
}

} // namespace engine::animation
