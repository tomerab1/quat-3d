#pragma once

// AnimStateMachine — states + parameterized transitions (Phase 5, Slice 5.6).
//
// Each state plays a clip. Transitions fire when a named float parameter meets a
// comparison, crossfading (linear pose blend) from the current state to the
// target over the transition's duration. update() advances time, evaluates
// transitions, and returns the skinning matrices for the current (or blended)
// pose.

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "engine/animation/animator.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/core/error.hpp"

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
    float         duration = 0.2F; // crossfade seconds
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
    [[nodiscard]] bool transitioning() const noexcept { return target_ >= 0; }

private:
    [[nodiscard]] bool condition_met(const AnimCondition& c) const;

    std::vector<AnimState>                   states_;
    std::vector<AnimTransition>              transitions_;
    std::unordered_map<std::string, float>   parameters_;

    int   current_ = -1;
    float current_time_ = 0.0F;
    int   target_ = -1;
    float target_time_ = 0.0F;
    float transition_elapsed_ = 0.0F;
    float transition_duration_ = 0.0F;
};

// Two states (idle/walk) with a parameter-driven crossfade; verifies the current
// state, the mid-crossfade blended pose, and that the transition completes.
[[nodiscard]] std::expected<void, core::Error> run_state_machine_self_test();

} // namespace engine::animation
