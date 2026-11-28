#pragma once

// Action maps (G5.1): named, typed actions bound to device controls, as data.
// A map is a JSON asset (docs/input.md has the full format):
//
//   {"version": 1,
//    "contexts": [{"name": "camera", "active": true}],
//    "actions": [
//      {"name": "camera.pan", "type": "axis2d", "context": "camera",
//       "bindings": [{"composite": "2d", "up": "key:w", "down": "key:s",
//                     "left": "key:a", "right": "key:d"},
//                    {"source": "gamepad:left_stick_xy", "modifiers": [{"dead_zone": 0.15}]}]},
//      {"name": "edit.undo", "type": "button", "bindings": ["key:ctrl+z"]},
//      {"name": "tool.primary", "type": "button", "trigger": {"drag": 4},
//       "bindings": ["mouse:left"]}]}
//
// Types: button (value in [0, 1]), axis1d (x), axis2d (x, y; y is "up" or
// "forward"). Sources: key:<name> with optional ctrl+ / shift+ / alt+ chord
// prefixes, mouse:<button>, mouse:delta / delta_x / delta_y (pixels this
// frame), mouse:wheel (steps this frame), pointer (viewport pixels), edge
// (screen-edge zones, a 2D direction), gamepad:<button>, gamepad:<axis>,
// gamepad:left_stick_xy / right_stick_xy (2D, up positive). 2D controls read
// by a 1D binding take a component suffix (_x / _y: mouse:delta_x, edge_y). A binding may also
// require another control to be held ("with": "mouse:right").
//
// Triggers decide when an action is active: down (default: while actuated),
// pressed / released (one frame), tap(max s), hold(min s), double_click(max
// gap s) and drag(dead zone px: a press that moves the pointer that far,
// then until release). Modifiers process values: dead_zone, scale, invert,
// smoothing (half-life, per action) and camera_relative (per action).
//
// Parsing is strict: maps are untrusted data (project files, remote requests)
// and are read through core::JsonFields, so every error names its field.

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "engine/core/error.hpp"

namespace engine::input {

enum class ActionType : std::uint8_t { button, axis1d, axis2d };

// The physical control a source reads.
enum class Control : std::uint8_t {
    none,
    key,            // index: HID usage id
    mouse_button,   // index: 0..4
    mouse_delta,    // 2D, pixels this frame (y down, as the mouse reports)
    mouse_wheel,    // 1D, steps this frame
    pointer,        // 2D, viewport pixels (y down)
    edge,           // 2D direction toward the screen edge the pointer is in (y up)
    gamepad_button, // index: SDL_GamepadButton
    gamepad_axis,   // 1D, index: SDL_GamepadAxis (sticks y down, as SDL reports)
    gamepad_stick,  // 2D, index 0 = left, 1 = right (y up)
};

// Which part of a 2D control a 1D read takes.
enum class Component : std::uint8_t { both, x, y };

// Keyboard chord modifiers (either side of the keyboard).
enum ChordBits : std::uint8_t { chord_ctrl = 1U, chord_shift = 2U, chord_alt = 4U };

struct Source {
    Control       control = Control::none;
    std::uint16_t index = 0;
    Component     component = Component::both;
    std::uint8_t  chord = 0; // ChordBits that must be held (key sources)
    // Screen-edge zones (edge only): zone width in pixels, and whether the
    // zones work in a window (default: fullscreen only).
    float edge_px = 8.0F;
    bool  edge_windowed = false;

    [[nodiscard]] bool valid() const { return control != Control::none; }
    bool operator==(const Source&) const = default;
};

enum class ModifierKind : std::uint8_t { dead_zone, scale, invert, smoothing, camera_relative };

struct Modifier {
    ModifierKind kind = ModifierKind::scale;
    // dead_zone: x = lower bound, y = upper bound (radial for 2D values);
    // scale: per axis; invert: 1 = flip that axis; smoothing: x = half-life (s).
    glm::vec2 value{1.0F};
    bool operator==(const Modifier&) const = default;
};

struct Binding {
    enum class Kind : std::uint8_t { single, composite1d, composite2d };
    Kind   kind = Kind::single;
    Source source;                 // single
    // composite1d: [negative, positive]; composite2d: [up, down, left, right].
    std::array<Source, 4> parts{};
    Source with;                   // another control that must be held (optional)
    std::vector<Modifier> modifiers;
    bool operator==(const Binding&) const = default;
};

enum class TriggerKind : std::uint8_t {
    down, pressed, released, tap, hold, double_click, drag
};

struct Trigger {
    TriggerKind kind = TriggerKind::down;
    // tap: max press duration (s); hold: min duration (s); double_click: max
    // gap between the presses (s); drag: dead zone (px).
    float param = 0.0F;
    bool operator==(const Trigger&) const = default;
};

struct ActionDesc {
    std::string           name;
    ActionType            type = ActionType::button;
    std::string           context = "default";
    std::vector<Binding>  bindings;
    std::vector<Modifier> modifiers; // after the binding's own
    Trigger               trigger;
    float                 press_point = 0.5F; // actuation threshold (magnitude)
};

struct ContextDesc {
    std::string name;
    bool active = false;    // pushed when the map is loaded
    bool exclusive = false; // while on the stack, contexts below it see no input
    bool consume = true;    // controls its active actions read are hidden from lower contexts
};

struct ActionMap {
    std::vector<ContextDesc> contexts;
    std::vector<ActionDesc>  actions;

    [[nodiscard]] int find_action(std::string_view name) const;
    [[nodiscard]] int find_context(std::string_view name) const;
};

// Limits on untrusted maps.
inline constexpr std::size_t k_max_actions = 512;
inline constexpr std::size_t k_max_contexts = 32;
inline constexpr std::size_t k_max_bindings = 16;
inline constexpr std::size_t k_max_modifiers = 8;

[[nodiscard]] std::expected<ActionMap, core::Error> parse_action_map(const nlohmann::json& json);
[[nodiscard]] nlohmann::json action_map_json(const ActionMap& map);

// One binding (a string such as "key:ctrl+z" or an object), and back. The
// encoder writes the short string form whenever the binding has nothing a
// string cannot say.
[[nodiscard]] std::expected<Binding, core::Error> parse_binding(const nlohmann::json& json,
                                                                std::string_view context);
[[nodiscard]] nlohmann::json binding_json(const Binding& binding);
[[nodiscard]] std::string source_string(const Source& source);
[[nodiscard]] std::string_view action_type_name(ActionType type);

// The engine's default map: G7's built-in axes and actions (move_x, move_y,
// look_x, look_y; jump, fire, aim, interact, sprint, pause) in an always
// active "default" context, so scripts written against G7 keep working.
[[nodiscard]] ActionMap default_action_map();

// Appends `extra`'s contexts and actions to `base`; an action or context
// whose name `base` already has replaces it there. Fails when the result
// exceeds the limits.
[[nodiscard]] std::expected<void, core::Error> merge_action_map(ActionMap& base, ActionMap extra);

} // namespace engine::input
