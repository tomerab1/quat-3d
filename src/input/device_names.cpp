// Input device name tables — see include/engine/input/device_state.hpp.

#include <array>
#include <string_view>

#include "engine/input/device_state.hpp"

namespace engine::input {

namespace {

struct Named {
    std::string_view name;
    std::size_t      index;
};

// USB HID usage ids (== SDL3 scancodes). Letters a..z are 4..29, digits
// 1..9, 0 are 30..39 and F1..F12 are 58..69; the rest are named here. The
// first spelling of an index is its canonical name.
constexpr std::array k_keys = {
    Named{"enter", 40},       Named{"return", 40},       Named{"escape", 41},
    Named{"backspace", 42},   Named{"tab", 43},          Named{"space", 44},
    Named{"minus", 45},       Named{"equals", 46},       Named{"left_bracket", 47},
    Named{"right_bracket", 48}, Named{"backslash", 49},  Named{"semicolon", 51},
    Named{"apostrophe", 52},  Named{"grave", 53},        Named{"comma", 54},
    Named{"period", 55},      Named{"slash", 56},        Named{"caps_lock", 57},
    Named{"insert", 73},      Named{"home", 74},         Named{"page_up", 75},
    Named{"delete", 76},      Named{"end", 77},          Named{"page_down", 78},
    Named{"right", 79},       Named{"left", 80},         Named{"down", 81},
    Named{"up", 82},          Named{"left_ctrl", 224},   Named{"left_shift", 225},
    Named{"left_alt", 226},   Named{"right_ctrl", 228},  Named{"right_shift", 229},
    Named{"right_alt", 230},
};

constexpr std::array<std::string_view, 26> k_letters = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
constexpr std::array<std::string_view, 10> k_digits = {"1", "2", "3", "4", "5",
                                                       "6", "7", "8", "9", "0"};
constexpr std::array<std::string_view, 12> k_function_keys = {
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12"};

constexpr std::array k_mouse = {Named{"left", 0}, Named{"middle", 1}, Named{"right", 2},
                                Named{"x1", 3}, Named{"x2", 4}};

// SDL_GamepadButton order (positional names plus Xbox-style aliases).
constexpr std::array k_gamepad_buttons = {
    Named{"south", 0},          Named{"a", 0},           Named{"east", 1},
    Named{"b", 1},              Named{"west", 2},        Named{"x", 2},
    Named{"north", 3},          Named{"y", 3},           Named{"back", 4},
    Named{"guide", 5},          Named{"start", 6},       Named{"left_stick", 7},
    Named{"right_stick", 8},    Named{"left_shoulder", 9}, Named{"right_shoulder", 10},
    Named{"dpad_up", 11},       Named{"dpad_down", 12},  Named{"dpad_left", 13},
    Named{"dpad_right", 14},
};

// SDL_GamepadAxis order.
constexpr std::array k_gamepad_axes = {Named{"left_x", 0},       Named{"left_y", 1},
                                       Named{"right_x", 2},      Named{"right_y", 3},
                                       Named{"left_trigger", 4}, Named{"right_trigger", 5}};

template <std::size_t N>
[[nodiscard]] std::optional<std::size_t> lookup(const std::array<Named, N>& table,
                                                std::string_view name) {
    for (const Named& n : table) {
        if (n.name == name) return n.index;
    }
    return std::nullopt;
}

template <std::size_t N>
[[nodiscard]] std::string_view reverse(const std::array<Named, N>& table, std::size_t index) {
    for (const Named& n : table) {
        if (n.index == index) return n.name;
    }
    return {};
}

template <std::size_t N>
[[nodiscard]] std::optional<std::size_t> position(const std::array<std::string_view, N>& table,
                                                  std::string_view name) {
    for (std::size_t i = 0; i < N; ++i) {
        if (table[i] == name) return i;
    }
    return std::nullopt;
}

constexpr std::size_t k_first_letter = 4;
constexpr std::size_t k_first_digit = 30;
constexpr std::size_t k_first_function = 58;

} // namespace

std::optional<std::size_t> key_from_name(std::string_view name) {
    if (const auto i = position(k_letters, name)) return k_first_letter + *i;
    if (const auto i = position(k_digits, name)) return k_first_digit + *i;
    if (const auto i = position(k_function_keys, name)) return k_first_function + *i;
    return lookup(k_keys, name);
}

std::string_view key_name(std::size_t index) {
    if (index >= k_first_letter && index < k_first_letter + k_letters.size()) {
        return k_letters[index - k_first_letter];
    }
    if (index >= k_first_digit && index < k_first_digit + k_digits.size()) {
        return k_digits[index - k_first_digit];
    }
    if (index >= k_first_function && index < k_first_function + k_function_keys.size()) {
        return k_function_keys[index - k_first_function];
    }
    return reverse(k_keys, index);
}

std::optional<std::size_t> mouse_button_from_name(std::string_view name) {
    return lookup(k_mouse, name);
}
std::string_view mouse_button_name(std::size_t index) { return reverse(k_mouse, index); }

std::optional<std::size_t> gamepad_button_from_name(std::string_view name) {
    return lookup(k_gamepad_buttons, name);
}
std::string_view gamepad_button_name(std::size_t index) { return reverse(k_gamepad_buttons, index); }

std::optional<std::size_t> gamepad_axis_from_name(std::string_view name) {
    return lookup(k_gamepad_axes, name);
}
std::string_view gamepad_axis_name(std::size_t index) { return reverse(k_gamepad_axes, index); }

} // namespace engine::input
