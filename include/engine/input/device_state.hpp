#pragma once

// DeviceState — one frame's raw input (G5): keyboard, mouse, the first
// gamepad, and the viewport the pointer is measured against. The application
// samples it from SDL3 once per frame (src/app/input_sampler); tests and the
// action evaluator use it as plain data, so nothing here depends on SDL.
//
// Keys are indexed by USB HID usage id, which is what SDL3 scancodes are
// (SDL_SCANCODE_A == 4). Gamepad buttons and axes follow SDL3's
// SDL_GamepadButton / SDL_GamepadAxis order. Gameplay scripts read this
// struct as `script::ScriptInput` (G7).

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <glm/glm.hpp>

namespace engine::input {

struct DeviceState {
    static constexpr std::size_t key_count = 512;
    static constexpr std::size_t mouse_button_count = 5;   // left, middle, right, x1, x2
    static constexpr std::size_t gamepad_button_count = 32;
    static constexpr std::size_t gamepad_axis_count = 6;   // left x/y, right x/y, triggers

    std::bitset<key_count> keys;      // held this frame
    std::bitset<key_count> keys_prev; // held last frame
    std::bitset<mouse_button_count> mouse;
    std::bitset<mouse_button_count> mouse_prev;
    // Window coordinates, as SDL reports them: points on a high-density
    // macOS / Wayland window (half the framebuffer's pixels at 2x), pixels on
    // Windows and X11. Only ever compared with viewport_* (same space), so
    // rays and edge zones do not depend on the pixel density.
    glm::vec2 mouse_position{0.0F}; // window coordinates
    glm::vec2 mouse_delta{0.0F};    // window coordinates moved this frame
    float     mouse_wheel = 0.0F;   // wheel steps this frame (+ away from the user)

    bool gamepad_connected = false;
    std::bitset<gamepad_button_count> gamepad;
    std::bitset<gamepad_button_count> gamepad_prev;
    // Sticks in [-1, 1] (y down positive, as SDL reports), triggers in [0, 1].
    std::array<float, gamepad_axis_count> gamepad_axes{};

    // The UI owns the keyboard this frame (a text field has focus): keyboard
    // controls read as released.
    bool keyboard_blocked = false;
    // The pointer is over UI (an editor panel, a G6 widget): pointer controls
    // (mouse buttons, wheel, motion, position, screen edges) cannot start an
    // action. One already active keeps reading them (mouse capture), so a
    // drag that leaves the viewport continues.
    bool pointer_blocked = false;

    // The rectangle the scene is drawn into, in window coordinates (the whole
    // window, or the editor's Viewport panel). Pointer positions and screen-
    // edge zones are measured against it; an empty size means the window.
    glm::vec2 viewport_origin{0.0F};
    glm::vec2 viewport_size{0.0F};
    // The window covers the screen (edge scrolling defaults to fullscreen
    // only) and has input focus.
    bool fullscreen = false;
    bool focused = true;

    // Starts a new frame: the current state becomes the previous one and the
    // per-frame deltas reset.
    void begin_frame() {
        keys_prev = keys;
        mouse_prev = mouse;
        gamepad_prev = gamepad;
        mouse_delta = glm::vec2(0.0F);
        mouse_wheel = 0.0F;
    }

    // The pointer relative to the viewport's top-left corner.
    [[nodiscard]] glm::vec2 pointer() const { return mouse_position - viewport_origin; }
};

// Name tables (docs/input.md lists them). nullopt for an unknown name.
[[nodiscard]] std::optional<std::size_t> key_from_name(std::string_view name);
[[nodiscard]] std::optional<std::size_t> mouse_button_from_name(std::string_view name);
[[nodiscard]] std::optional<std::size_t> gamepad_button_from_name(std::string_view name);
[[nodiscard]] std::optional<std::size_t> gamepad_axis_from_name(std::string_view name);
// The canonical name of an index (the first spelling in each table); empty
// when it has none.
[[nodiscard]] std::string_view key_name(std::size_t index);
[[nodiscard]] std::string_view mouse_button_name(std::size_t index);
[[nodiscard]] std::string_view gamepad_button_name(std::size_t index);
[[nodiscard]] std::string_view gamepad_axis_name(std::size_t index);

} // namespace engine::input
