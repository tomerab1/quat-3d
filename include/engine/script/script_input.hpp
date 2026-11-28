#pragma once

// ScriptInput — the input state gameplay scripts read (G7), sampled once per
// frame by the application from SDL3 (keyboard, mouse, first gamepad). Plain
// data so the script runtime stays independent of the windowing layer and
// tests can drive it directly.
//
// Keys are indexed by USB HID usage id, which is what SDL3 scancodes are
// (SDL_SCANCODE_A == 4). Gamepad buttons and axes follow SDL3's
// SDL_GamepadButton / SDL_GamepadAxis order.

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <glm/glm.hpp>

namespace engine::script {

struct ScriptInput {
    static constexpr std::size_t key_count = 512;
    static constexpr std::size_t mouse_button_count = 5;   // left, middle, right, x1, x2
    static constexpr std::size_t gamepad_button_count = 32;
    static constexpr std::size_t gamepad_axis_count = 6;   // left x/y, right x/y, triggers

    std::bitset<key_count> keys;      // held this frame
    std::bitset<key_count> keys_prev; // held last frame
    std::bitset<mouse_button_count> mouse;
    std::bitset<mouse_button_count> mouse_prev;
    glm::vec2 mouse_position{0.0F}; // pixels, window space
    glm::vec2 mouse_delta{0.0F};    // pixels moved this frame
    float     mouse_wheel = 0.0F;   // wheel steps this frame (+ away from the user)

    bool gamepad_connected = false;
    std::bitset<gamepad_button_count> gamepad;
    std::bitset<gamepad_button_count> gamepad_prev;
    // Sticks in [-1, 1] (y down positive, as SDL reports), triggers in [0, 1].
    std::array<float, gamepad_axis_count> gamepad_axes{};

    // The editor UI owns the keyboard this frame (a text field has focus):
    // keys, movement axes and actions read as released.
    bool keyboard_blocked = false;

    // Starts a new frame: the current state becomes the previous one and the
    // per-frame deltas reset.
    void begin_frame() {
        keys_prev = keys;
        mouse_prev = mouse;
        gamepad_prev = gamepad;
        mouse_delta = glm::vec2(0.0F);
        mouse_wheel = 0.0F;
    }
};

// Name tables (see docs/scripting.md for the lists). nullopt for an unknown
// name.
[[nodiscard]] std::optional<std::size_t> key_from_name(std::string_view name);
[[nodiscard]] std::optional<std::size_t> mouse_button_from_name(std::string_view name);
[[nodiscard]] std::optional<std::size_t> gamepad_button_from_name(std::string_view name);
[[nodiscard]] std::optional<std::size_t> gamepad_axis_from_name(std::string_view name);

// Virtual axes and actions combining keyboard, mouse and gamepad bindings
// ("move_x", "jump", ...). nullopt for an unknown name.
[[nodiscard]] std::optional<float> input_axis(const ScriptInput& input, std::string_view name);
enum class ActionPhase : std::uint8_t { held, pressed, released };
[[nodiscard]] std::optional<bool> input_action(const ScriptInput& input, std::string_view name,
                                               ActionPhase phase);

} // namespace engine::script
