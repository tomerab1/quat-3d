#pragma once

// Private to engine_input: the pieces of the action-map codec shared between
// its files.

#include <array>
#include <bitset>
#include <cstdint>
#include <expected>
#include <string>
#include <span>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"
#include "engine/input/action_map.hpp"
#include "engine/input/device_state.hpp"

namespace engine::input {

// "key:ctrl+z", "mouse:delta_x", "gamepad:left_stick_xy", ... (docs/input.md).
[[nodiscard]] std::expected<Source, std::string> parse_source(std::string_view text);
// Whether the source reads two components.
[[nodiscard]] bool is_2d(const Source& source);

// Reads a modifier list at `key`; smoothing and camera_relative are only
// accepted at action level.
void read_modifiers(core::JsonFields& f, std::string_view key, std::vector<Modifier>& out,
                    bool action_level);
[[nodiscard]] nlohmann::json modifiers_json(const std::vector<Modifier>& modifiers);

// ---- Evaluation (source_read.cpp) -------------------------------------------

// Controls hidden from lower contexts by the ones above them.
struct Consumed {
    std::bitset<DeviceState::key_count>            keys;
    std::bitset<DeviceState::mouse_button_count>   mouse;
    std::bitset<DeviceState::gamepad_button_count> pad_buttons;
    std::bitset<DeviceState::gamepad_axis_count>   pad_axes;
    bool delta = false;
    bool wheel = false;
    bool pointer = false; // pointer position and screen edges
    bool all = false;     // below an exclusive context

    [[nodiscard]] bool has(const Source& s) const;
    void add(const Source& s);
};

// What one binding read sees.
struct ReadContext {
    const DeviceState* device = nullptr;
    const Consumed*    consumed = nullptr;
    // Per key: the most chord modifiers of a satisfied chord on it (a source
    // with fewer reads released).
    const std::array<std::uint8_t, DeviceState::key_count>* chord_best = nullptr;
    bool pointer_allowed = true; // pointer_blocked is off, or the action holds the pointer
};

[[nodiscard]] bool is_pointer_control(Control c);
// ChordBits held on the device.
[[nodiscard]] std::uint8_t held_chord(const DeviceState& device);
// Fills `best` from every chord binding of the given actions.
void chord_priorities(const DeviceState& device, std::span<const ActionDesc* const> actions,
                      std::array<std::uint8_t, DeviceState::key_count>& best);
// The binding's value (x for 1D), its own modifiers applied. `pointer_used`
// is set when a pointer control contributed.
[[nodiscard]] glm::vec2 read_binding(const Binding& binding, const ReadContext& ctx,
                                     bool& pointer_used);
void consume_binding(const Binding& binding, Consumed& consumed);
// Stateless modifiers (dead zone, scale, invert) applied to `v`.
void apply_modifier(const Modifier& m, glm::vec2& v);

} // namespace engine::input
