// The engine's default action map: G7's built-in virtual axes and actions —
// see default_action_map() in include/engine/input/action_map.hpp.

#include <string>
#include <utility>

#include "engine/input/action_map.hpp"
#include "engine/input/device_state.hpp"

namespace engine::input {

namespace {

[[nodiscard]] Source key(std::string_view name) {
    Source s;
    s.control = Control::key;
    s.index = static_cast<std::uint16_t>(key_from_name(name).value_or(0));
    return s;
}

[[nodiscard]] Source pad_button(std::string_view name) {
    Source s;
    s.control = Control::gamepad_button;
    s.index = static_cast<std::uint16_t>(gamepad_button_from_name(name).value_or(0));
    return s;
}

[[nodiscard]] Source pad_axis(std::string_view name) {
    Source s;
    s.control = Control::gamepad_axis;
    s.index = static_cast<std::uint16_t>(gamepad_axis_from_name(name).value_or(0));
    return s;
}

[[nodiscard]] Source mouse(std::string_view name) {
    Source s;
    s.control = Control::mouse_button;
    s.index = static_cast<std::uint16_t>(mouse_button_from_name(name).value_or(0));
    return s;
}

[[nodiscard]] Binding single(Source s, std::vector<Modifier> mods = {}) {
    Binding b;
    b.source = s;
    b.modifiers = std::move(mods);
    return b;
}

[[nodiscard]] Binding composite(Source negative, Source positive) {
    Binding b;
    b.kind = Binding::Kind::composite1d;
    b.parts[0] = negative;
    b.parts[1] = positive;
    return b;
}

// G7's stick dead zone (rescaled, so the output still spans [0, 1]).
const Modifier k_stick_dead_zone{ModifierKind::dead_zone, glm::vec2(0.15F, 1.0F)};
const Modifier k_flip{ModifierKind::invert, glm::vec2(1.0F, 0.0F)};

[[nodiscard]] ActionDesc action(std::string name, ActionType type, std::vector<Binding> bindings) {
    ActionDesc a;
    a.name = std::move(name);
    a.type = type;
    a.bindings = std::move(bindings);
    return a;
}

} // namespace

ActionMap default_action_map() {
    ActionMap map;
    map.contexts.push_back(ContextDesc{"default", true, false, true});
    const ActionType axis = ActionType::axis1d;
    const ActionType button = ActionType::button;
    map.actions = {
        // Stick y is down-positive as SDL reports it: forward / up flips it.
        action("move_x", axis, {composite(key("a"), key("d")), composite(key("left"), key("right")),
                                single(pad_axis("left_x"), {k_stick_dead_zone})}),
        action("move_y", axis, {composite(key("s"), key("w")), composite(key("down"), key("up")),
                                single(pad_axis("left_y"), {k_stick_dead_zone, k_flip})}),
        action("look_x", axis, {single(pad_axis("right_x"), {k_stick_dead_zone})}),
        action("look_y", axis, {single(pad_axis("right_y"), {k_stick_dead_zone, k_flip})}),
        action("jump", button, {single(key("space")), single(pad_button("south"))}),
        action("fire", button, {single(key("left_ctrl")), single(key("right_ctrl")),
                                single(mouse("left")), single(pad_axis("right_trigger"))}),
        action("aim", button, {single(mouse("right")), single(pad_axis("left_trigger"))}),
        action("interact", button, {single(key("e")), single(pad_button("west"))}),
        action("sprint", button, {single(key("left_shift")), single(key("right_shift")),
                                  single(pad_button("left_stick"))}),
        action("pause", button, {single(key("p")), single(pad_button("start"))}),
    };
    return map;
}

} // namespace engine::input
