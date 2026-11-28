// Input name tables and virtual axes/actions — see include/engine/script/script_input.hpp.

#include "engine/script/script_input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace engine::script {

namespace {

struct Named {
    std::string_view name;
    std::size_t      index;
};

// USB HID usage ids (== SDL3 scancodes) of the named non-alphanumeric keys.
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

constexpr float k_stick_dead_zone = 0.15F;

[[nodiscard]] float dead_zone(float v) {
    const float a = std::abs(v);
    if (a <= k_stick_dead_zone) return 0.0F;
    return std::copysign((a - k_stick_dead_zone) / (1.0F - k_stick_dead_zone), v);
}

[[nodiscard]] bool key(const ScriptInput& in, std::size_t k, bool prev = false) {
    if (k >= ScriptInput::key_count) return false;
    return prev ? in.keys_prev.test(k) : in.keys.test(k);
}

[[nodiscard]] float key_axis(const ScriptInput& in, std::size_t neg_a, std::size_t neg_b,
                             std::size_t pos_a, std::size_t pos_b) {
    const float neg = (key(in, neg_a) || key(in, neg_b)) ? 1.0F : 0.0F;
    const float pos = (key(in, pos_a) || key(in, pos_b)) ? 1.0F : 0.0F;
    return pos - neg;
}

// Letters a..z are usage ids 4..29; digits 1..9, 0 are 30..39.
constexpr std::size_t k_a = 4, k_d = 7, k_e = 8, k_s = 22, k_w = 26;
constexpr std::size_t k_space = 44, k_left = 80, k_right = 79, k_up = 82, k_down = 81;
constexpr std::size_t k_lshift = 225, k_rshift = 229, k_lctrl = 224, k_rctrl = 228;

struct ActionBinding {
    std::string_view name;
    std::size_t      key_a;
    std::size_t      key_b;
    int              mouse;   // -1: none
    int              gamepad; // -1: none
    int              trigger; // gamepad axis read as a button (> 0.5); -1: none
};

constexpr std::array k_actions = {
    ActionBinding{"jump", k_space, k_space, -1, 0, -1},
    ActionBinding{"fire", k_lctrl, k_rctrl, 0, -1, 5},
    ActionBinding{"aim", ScriptInput::key_count, ScriptInput::key_count, 2, -1, 4},
    ActionBinding{"interact", k_e, k_e, -1, 2, -1},
    ActionBinding{"sprint", k_lshift, k_rshift, -1, 7, -1},
    ActionBinding{"pause", 19, 19, -1, 6, -1}, // P (Escape quits the game)
};

[[nodiscard]] bool action_state(const ScriptInput& in, const ActionBinding& b, bool prev) {
    if (key(in, b.key_a, prev) || key(in, b.key_b, prev)) return true;
    if (b.mouse >= 0) {
        const auto m = static_cast<std::size_t>(b.mouse);
        if (prev ? in.mouse_prev.test(m) : in.mouse.test(m)) return true;
    }
    if (!in.gamepad_connected) return false;
    if (b.gamepad >= 0) {
        const auto g = static_cast<std::size_t>(b.gamepad);
        if (prev ? in.gamepad_prev.test(g) : in.gamepad.test(g)) return true;
    }
    // Triggers have no previous-frame sample; a trigger counts as held only.
    return !prev && b.trigger >= 0 &&
           in.gamepad_axes[static_cast<std::size_t>(b.trigger)] > 0.5F;
}

} // namespace

std::optional<std::size_t> key_from_name(std::string_view name) {
    if (name.size() == 1) {
        const char c = name.front();
        if (c >= 'a' && c <= 'z') return static_cast<std::size_t>(4 + (c - 'a'));
        if (c >= '1' && c <= '9') return static_cast<std::size_t>(30 + (c - '1'));
        if (c == '0') return 39U;
    }
    if (name.size() >= 2 && name.size() <= 3 && name.front() == 'f') {
        int n = 0;
        for (char c : name.substr(1)) {
            if (c < '0' || c > '9') return lookup(k_keys, name);
            n = n * 10 + (c - '0');
        }
        if (n >= 1 && n <= 12) return static_cast<std::size_t>(58 + n - 1);
    }
    return lookup(k_keys, name);
}

std::optional<std::size_t> mouse_button_from_name(std::string_view name) {
    return lookup(k_mouse, name);
}

std::optional<std::size_t> gamepad_button_from_name(std::string_view name) {
    return lookup(k_gamepad_buttons, name);
}

std::optional<std::size_t> gamepad_axis_from_name(std::string_view name) {
    return lookup(k_gamepad_axes, name);
}

std::optional<float> input_axis(const ScriptInput& in, std::string_view name) {
    const auto pad = [&](std::size_t axis) {
        return in.gamepad_connected ? dead_zone(in.gamepad_axes[axis]) : 0.0F;
    };
    float v = 0.0F;
    if (name == "move_x") {
        v = key_axis(in, k_a, k_left, k_d, k_right) + pad(0);
    } else if (name == "move_y") { // forward positive
        v = key_axis(in, k_s, k_down, k_w, k_up) - pad(1);
    } else if (name == "look_x") {
        v = pad(2);
    } else if (name == "look_y") { // up positive
        v = -pad(3);
    } else {
        return std::nullopt;
    }
    return std::clamp(v, -1.0F, 1.0F);
}

std::optional<bool> input_action(const ScriptInput& in, std::string_view name, ActionPhase phase) {
    for (const ActionBinding& b : k_actions) {
        if (b.name != name) continue;
        const bool now = action_state(in, b, false);
        const bool before = action_state(in, b, true);
        switch (phase) {
        case ActionPhase::held: return now;
        case ActionPhase::pressed: return now && !before;
        case ActionPhase::released: return !now && before;
        }
    }
    return std::nullopt;
}

} // namespace engine::script
