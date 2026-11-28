#pragma once

// InputSampler — fills the ScriptInput gameplay scripts read (G7, app layer)
// from SDL3 once per frame: keyboard state, mouse buttons / position /
// motion / wheel, and the first connected gamepad (opened when it appears).
//
// Frame use: begin_frame() before polling events, process_event() for each
// event, sample() after the event loop.

#include "engine/script/script_input.hpp"

struct SDL_Gamepad;
struct SDL_Window;
union SDL_Event;

namespace quat::app {

class InputSampler {
public:
    InputSampler() = default;
    InputSampler(const InputSampler&) = delete;
    InputSampler& operator=(const InputSampler&) = delete;
    ~InputSampler();

    // Opens the gamepad subsystem (failure leaves gamepads unavailable).
    void init();
    void begin_frame() { state_.begin_frame(); }
    void process_event(const SDL_Event& event);
    // Reads the keyboard / mouse state. `keyboard_blocked`: a UI text field
    // owns the keyboard this frame.
    void sample(bool keyboard_blocked);

    [[nodiscard]] const engine::script::ScriptInput& state() const { return state_; }

private:
    engine::script::ScriptInput state_;
    SDL_Gamepad*                gamepad_ = nullptr;
    bool                        gamepad_subsystem_ = false;
};

} // namespace quat::app
