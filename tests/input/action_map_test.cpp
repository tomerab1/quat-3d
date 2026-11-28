// Action map codec (G5.1): sources, bindings, modifiers, triggers, contexts,
// validation errors and round trips.

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/input/action_map.hpp"
#include "engine/input/device_state.hpp"
#include "support/expect_ok.hpp"

namespace quat::test {
namespace {

using engine::input::ActionMap;
using engine::input::ActionType;
using engine::input::Binding;
using engine::input::Control;
using engine::input::TriggerKind;
using nlohmann::json;

[[nodiscard]] std::expected<ActionMap, engine::core::Error> parse(const char* text) {
    return engine::input::parse_action_map(json::parse(text));
}

TEST(InputActionMap, ParsesSourcesBindingsAndTriggers) {
    auto map = parse(R"({
        "version": 1,
        "contexts": [{"name": "camera", "active": true}, {"name": "tool", "exclusive": true}],
        "actions": [
          {"name": "camera.pan", "type": "axis2d", "context": "camera",
           "bindings": [{"composite": "2d", "up": "key:w", "down": "key:s", "left": "key:a", "right": "key:d"},
                        {"source": "gamepad:left_stick_xy", "modifiers": [{"dead_zone": 0.15}]},
                        {"source": "edge", "edge_px": 12, "windowed": true}],
           "modifiers": ["camera_relative"]},
          {"name": "camera.orbit", "type": "axis1d", "context": "camera",
           "bindings": [{"composite": "1d", "negative": "key:q", "positive": "key:e"},
                        {"source": "mouse:delta_x", "with": "mouse:right", "modifiers": [{"scale": 0.25}]}]},
          {"name": "edit.undo", "type": "button", "bindings": ["key:ctrl+z"]},
          {"name": "tool.primary", "type": "button", "context": "tool", "trigger": {"drag": 6},
           "bindings": ["mouse:left"]},
          {"name": "tool.place", "type": "button", "context": "tool", "trigger": "double_click",
           "bindings": ["mouse:left"]}
        ]})");
    ASSERT_OK(map);
    ASSERT_EQ(map->contexts.size(), 3U) << "the implicit default context is added first";
    EXPECT_EQ(map->contexts[0].name, "default");
    EXPECT_TRUE(map->contexts[2].exclusive);

    const auto& pan = map->actions[static_cast<std::size_t>(map->find_action("camera.pan"))];
    EXPECT_EQ(pan.type, ActionType::axis2d);
    ASSERT_EQ(pan.bindings.size(), 3U);
    EXPECT_EQ(pan.bindings[0].kind, Binding::Kind::composite2d);
    EXPECT_EQ(pan.bindings[0].parts[0].index, *engine::input::key_from_name("w"));
    EXPECT_EQ(pan.bindings[1].source.control, Control::gamepad_stick);
    EXPECT_EQ(pan.bindings[2].source.edge_px, 12.0F);
    EXPECT_TRUE(pan.bindings[2].source.edge_windowed);

    const auto& orbit = map->actions[1];
    EXPECT_EQ(orbit.bindings[1].source.control, Control::mouse_delta);
    EXPECT_EQ(orbit.bindings[1].with.control, Control::mouse_button);

    const auto& undo = map->actions[2];
    EXPECT_EQ(undo.context, "default");
    EXPECT_EQ(undo.bindings[0].source.chord, engine::input::chord_ctrl);

    EXPECT_EQ(map->actions[3].trigger.kind, TriggerKind::drag);
    EXPECT_FLOAT_EQ(map->actions[3].trigger.param, 6.0F);
    EXPECT_EQ(map->actions[4].trigger.kind, TriggerKind::double_click);
    EXPECT_FLOAT_EQ(map->actions[4].trigger.param, 0.3F) << "the documented default gap";
}

TEST(InputActionMap, RoundTripsThroughJson) {
    const ActionMap original = engine::input::default_action_map();
    auto again = engine::input::parse_action_map(engine::input::action_map_json(original));
    ASSERT_OK(again);
    ASSERT_EQ(again->actions.size(), original.actions.size());
    for (std::size_t i = 0; i < original.actions.size(); ++i) {
        EXPECT_EQ(again->actions[i].name, original.actions[i].name);
        EXPECT_EQ(again->actions[i].bindings, original.actions[i].bindings) << original.actions[i].name;
        EXPECT_EQ(again->actions[i].trigger, original.actions[i].trigger);
    }
    // Short string form whenever a binding has nothing else to say.
    EXPECT_EQ(engine::input::binding_json(original.actions[4].bindings[0]), json("key:space"));
}

TEST(InputActionMap, SourceStringsRoundTrip) {
    for (const char* text : {"key:ctrl+shift+z", "key:f11", "key:left_shift", "mouse:x1",
                             "mouse:delta", "mouse:delta_y", "mouse:wheel", "pointer", "pointer_x",
                             "edge", "gamepad:south", "gamepad:left_trigger",
                             "gamepad:right_stick_xy", "gamepad:left_stick_y", "gamepad:left_stick"}) {
        auto b = engine::input::parse_binding(json(text), "b");
        ASSERT_OK(b) << text;
        EXPECT_EQ(engine::input::source_string(b->source), text);
    }
}

TEST(InputActionMap, RejectsBadMapsWithPreciseErrors) {
    const auto error_of = [](const char* text) {
        auto m = parse(text);
        return m ? std::string() : m.error().message;
    };
    EXPECT_NE(error_of(R"({"actions": [{"name": "a", "type": "button", "bindings": ["key:hyperspace"]}]})")
                  .find("actions[0].bindings[0]: unknown key 'hyperspace'"),
              std::string::npos);
    EXPECT_NE(error_of(R"({"actions": [{"name": "a", "type": "trigger"}]})").find("actions[0].type"),
              std::string::npos);
    EXPECT_NE(error_of(R"({"actions": [{"name": "a", "type": "button", "context": "ui"}]})")
                  .find("unknown context 'ui'"),
              std::string::npos);
    EXPECT_NE(error_of(R"({"actions": [{"name": "a", "type": "button"}, {"name": "a", "type": "axis1d"}]})")
                  .find("duplicate action 'a'"),
              std::string::npos);
    EXPECT_NE(error_of(R"({"actions": [{"name": "Bad Name", "type": "button"}]})").find("actions[0].name"),
              std::string::npos);
    EXPECT_NE(error_of(R"({"actions": [{"name": "a", "type": "button", "trigger": {"hold": 99}}]})")
                  .find("trigger"),
              std::string::npos);
    EXPECT_NE(error_of(R"({"actions": [{"name": "a", "type": "button", "colour": 1}]})")
                  .find("unknown field"),
              std::string::npos);
    EXPECT_NE(error_of(R"({"actions": [{"name": "a", "type": "axis1d",
                         "bindings": [{"source": "mouse:delta_x", "modifiers": [{"smoothing": 0.1}]}]}]})")
                  .find("go on the action"),
              std::string::npos)
        << "stateful modifiers belong to the action";
    EXPECT_NE(error_of(R"({"actions": [{"name": "a", "type": "axis1d", "modifiers": ["camera_relative"]}]})")
                  .find("needs an axis2d"),
              std::string::npos);
    EXPECT_NE(error_of(R"({"actions": [{"name": "a", "type": "axis2d",
                         "bindings": [{"composite": "2d", "up": "mouse:delta"}]}]})")
                  .find("must be 1D"),
              std::string::npos);
    EXPECT_NE(error_of(R"({"actions": 3})").find("actions"), std::string::npos);
}

TEST(InputActionMap, MergeReplacesByName) {
    ActionMap base = engine::input::default_action_map();
    auto extra = parse(R"({"contexts": [{"name": "camera", "active": true}],
                           "actions": [{"name": "jump", "type": "button", "bindings": ["key:j"]},
                                       {"name": "camera.zoom", "type": "axis1d", "context": "camera",
                                        "bindings": ["mouse:wheel"]}]})");
    ASSERT_OK(extra);
    ASSERT_OK(engine::input::merge_action_map(base, *extra));
    EXPECT_EQ(base.actions.size(), engine::input::default_action_map().actions.size() + 1);
    const auto& jump = base.actions[static_cast<std::size_t>(base.find_action("jump"))];
    EXPECT_EQ(engine::input::binding_json(jump.bindings[0]), json("key:j"));
    EXPECT_GE(base.find_context("camera"), 0);
}

} // namespace
} // namespace quat::test
