#pragma once

// AnimStateMachine — states + parameterized transitions (Phase 5, Slice 5.6).
//
// Each state plays a clip. Transitions fire when a named float parameter meets a
// comparison and move to the target over the transition's duration, either by
// inertialization (only the target is evaluated; the outgoing pose's offset
// decays, see inertialization.hpp) or by a linear crossfade of both. update()
// advances time, evaluates transitions, and returns the skinning matrices.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "engine/animation/animator.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/inertialization.hpp"
#include "engine/animation/skeleton.hpp"

namespace engine::animation {

struct AnimState {
    std::string          name;
    const AnimClipAsset* clip = nullptr;
    float                speed = 1.0F;
    bool                 looping = true;
};

// Fires when parameter `parameter` is greater/less than `threshold`.
struct AnimCondition {
    std::string                  parameter;
    enum class Op : std::uint8_t { greater, less } op = Op::greater;
    float                        threshold = 0.5F;
};

struct AnimTransition {
    int           from = -1; // source state index, -1 = any
    int           to = 0;    // target state index
    AnimCondition condition;
    float         duration = 0.2F; // blend seconds
    BlendMode     blend = BlendMode::inertialize;
};

class AnimStateMachine {
public:
    int  add_state(AnimState state);          // returns the state index
    void add_transition(AnimTransition trans);
    void set_parameter(const std::string& name, float value);
    void set_current(int state);

    // Advance by `dt` seconds: progress clip time(s), evaluate transitions, and
    // return the per-joint skinning matrices for the resulting pose.
    [[nodiscard]] std::vector<glm::mat4> update(const SkeletonAsset& skeleton, float dt);

    [[nodiscard]] int  current_state() const noexcept { return current_; }
    [[nodiscard]] bool transitioning() const noexcept {
        return target_ >= 0 || inertializer_.active();
    }
    // Clip samples the last update() took (1 while inertializing, 2 while
    // crossfading; a transition's first frame samples its target twice).
    [[nodiscard]] int evaluations() const noexcept { return evaluations_; }

private:
    [[nodiscard]] bool condition_met(const AnimCondition& c) const;
    [[nodiscard]] std::vector<JointPose> state_pose(const SkeletonAsset& skeleton, int state,
                                                    float time);
    // Fires the first transition whose condition holds; returns the target
    // pose already sampled for an inertialized one.
    [[nodiscard]] std::vector<JointPose> begin_transition(const SkeletonAsset& skeleton, float dt);

    std::vector<AnimState>                   states_;
    std::vector<AnimTransition>              transitions_;
    std::unordered_map<std::string, float>   parameters_;

    int   current_ = -1;
    float current_time_ = 0.0F;
    int   target_ = -1;
    float target_time_ = 0.0F;
    float transition_elapsed_ = 0.0F;
    float transition_duration_ = 0.0F;

    Inertializer           inertializer_;
    std::vector<JointPose> last_pose_; // previous update()'s output
    std::vector<JointPose> prev_pose_; // the one before it
    float                  last_dt_ = 0.0F;
    int                    evaluations_ = 0;
};

} // namespace engine::animation
