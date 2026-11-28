#include "app/input_sampler.hpp"

#include <algorithm>
#include <cstdio>

#include <SDL3/SDL.h>

namespace quat::app {

InputSampler::~InputSampler() {
    if (gamepad_ != nullptr) SDL_CloseGamepad(gamepad_);
    if (gamepad_subsystem_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void InputSampler::init() {
    gamepad_subsystem_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!gamepad_subsystem_) {
        std::fprintf(stderr, "[input] no gamepad support: %s\n", SDL_GetError());
    }
}

void InputSampler::process_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        state_.mouse_delta += glm::vec2(event.motion.xrel, event.motion.yrel);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        state_.mouse_wheel += event.wheel.y;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (gamepad_ == nullptr) {
            gamepad_ = SDL_OpenGamepad(event.gdevice.which);
            if (gamepad_ != nullptr) {
                std::fprintf(stderr, "[input] gamepad connected: %s\n",
                             SDL_GetGamepadName(gamepad_) != nullptr ? SDL_GetGamepadName(gamepad_)
                                                                      : "unnamed");
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (gamepad_ != nullptr && SDL_GetGamepadID(gamepad_) == event.gdevice.which) {
            SDL_CloseGamepad(gamepad_);
            gamepad_ = nullptr;
            std::fprintf(stderr, "[input] gamepad disconnected\n");
        }
        break;
    default:
        break;
    }
}

void InputSampler::sample(SDL_Window* window, bool keyboard_blocked, bool pointer_blocked) {
    state_.pointer_blocked = pointer_blocked;
    if (window != nullptr) {
        int w = 0;
        int h = 0;
        (void)SDL_GetWindowSize(window, &w, &h);
        state_.viewport_origin = glm::vec2(0.0F);
        state_.viewport_size = glm::vec2(static_cast<float>(w), static_cast<float>(h));
        const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
        state_.fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
        state_.focused = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
    }
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    state_.keys.reset();
    const int n = std::min(count, static_cast<int>(engine::script::ScriptInput::key_count));
    for (int i = 0; keys != nullptr && i < n; ++i) {
        if (keys[i]) state_.keys.set(static_cast<std::size_t>(i));
    }
    state_.keyboard_blocked = keyboard_blocked;

    float x = 0.0F;
    float y = 0.0F;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
    state_.mouse_position = glm::vec2(x, y);
    state_.mouse.reset();
    // SDL buttons 1..5 (left, middle, right, x1, x2) -> bits 0..4.
    for (std::size_t b = 0; b < engine::script::ScriptInput::mouse_button_count; ++b) {
        if ((buttons & SDL_BUTTON_MASK(static_cast<int>(b) + 1)) != 0) state_.mouse.set(b);
    }

    state_.gamepad_connected = gamepad_ != nullptr;
    state_.gamepad.reset();
    state_.gamepad_axes.fill(0.0F);
    if (gamepad_ == nullptr) return;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT &&
                    b < static_cast<int>(engine::script::ScriptInput::gamepad_button_count);
         ++b) {
        if (SDL_GetGamepadButton(gamepad_, static_cast<SDL_GamepadButton>(b))) {
            state_.gamepad.set(static_cast<std::size_t>(b));
        }
    }
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT &&
                    a < static_cast<int>(engine::script::ScriptInput::gamepad_axis_count);
         ++a) {
        const Sint16 v = SDL_GetGamepadAxis(gamepad_, static_cast<SDL_GamepadAxis>(a));
        state_.gamepad_axes[static_cast<std::size_t>(a)] =
            std::clamp(static_cast<float>(v) / 32767.0F, -1.0F, 1.0F);
    }
}

} // namespace quat::app
