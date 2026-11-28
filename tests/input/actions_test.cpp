// input::Actions (G5.1): binding resolution, composites, chords, context
// priority and consumption, triggers, pointer capture, edge zones, the
// simulation sample, injection and determinism.

#include <cmath>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/input/actions.hpp"
#include "engine/input/device_state.hpp"
#include "support/expect_ok.hpp"

namespace quat::test {
namespace {

using engine::input::Actions;
using engine::input::DeviceState;
using nlohmann::json;

constexpr float k_dt = 1.0F / 60.0F;

[[nodiscard]] std::size_t key(const char* name) { return *engine::input::key_from_name(name); }

[[nodiscard]] Actions make(const char* map_json) {
    auto map = engine::input::parse_action_map(json::parse(map_json));
    EXPECT_TRUE(map.has_value()) << (map ? "" : map.error().message);
    return Actions(map ? *map : engine::input::ActionMap{});
}

// One frame: the device's previous state rolls over, `edit` sets this one.
template <typename F>
void frame(Actions& a, DeviceState& d, F&& edit, float dt = k_dt) {
    d.begin_frame();
    edit(d);
    a.update(d, dt);
}

TEST(InputActions, DefaultMapKeepsG7Names) {
    Actions a;
    DeviceState d;
    frame(a, d, [](DeviceState& s) {
        s.keys.set(key("w"));
        s.keys.set(key("space"));
    });
    EXPECT_FLOAT_EQ(a.state("move_y")->value.x, 1.0F);
    EXPECT_FLOAT_EQ(a.state("move_x")->value.x, 0.0F);
    EXPECT_TRUE(a.state("jump")->active);
    EXPECT_TRUE(a.state("jump")->started);
    frame(a, d, [](DeviceState&) {}); // keys stay held
    EXPECT_TRUE(a.state("jump")->active);
    EXPECT_FALSE(a.state("jump")->started);
    frame(a, d, [](DeviceState& s) { s.keys.reset(); });
    EXPECT_TRUE(a.state("jump")->ended);

    // Gamepad: stick y is down-positive from SDL, forward reads positive,
    // inside the dead zone reads zero, and the trigger works as a button.
    frame(a, d, [](DeviceState& s) {
        s.gamepad_connected = true;
        s.gamepad_axes[1] = -1.0F;
        s.gamepad_axes[0] = 0.1F;
        s.gamepad_axes[5] = 0.9F;
    });
    EXPECT_FLOAT_EQ(a.state("move_y")->value.x, 1.0F);
    EXPECT_FLOAT_EQ(a.state("move_x")->value.x, 0.0F);
    EXPECT_TRUE(a.state("fire")->started);
}

TEST(InputActions, CompositesNormaliseAndStrongestBindingWins) {
    Actions a = make(R"({"actions": [{"name": "pan", "type": "axis2d", "bindings": [
        {"composite": "2d", "up": "key:w", "down": "key:s", "left": "key:a", "right": "key:d"},
        "gamepad:left_stick_xy"]}]})");
    DeviceState d;
    frame(a, d, [](DeviceState& s) {
        s.keys.set(key("w"));
        s.keys.set(key("d"));
    });
    const glm::vec2 v = a.state("pan")->value;
    EXPECT_NEAR(glm::length(v), 1.0F, 1e-6F) << "a diagonal is not faster";
    EXPECT_NEAR(v.x, v.y, 1e-6F);
    frame(a, d, [](DeviceState& s) {
        s.keys.reset();
        s.keys.set(key("a"));
        s.gamepad_connected = true;
        s.gamepad_axes[0] = 0.3F; // weaker than the key
    });
    EXPECT_FLOAT_EQ(a.state("pan")->value.x, -1.0F);
    frame(a, d, [](DeviceState& s) { s.keys.reset(); });
    EXPECT_FLOAT_EQ(a.state("pan")->value.x, 0.3F);
}

TEST(InputActions, ChordBeatsPlainKey) {
    Actions a = make(R"({"actions": [
        {"name": "undo", "type": "button", "bindings": ["key:ctrl+z"]},
        {"name": "redo", "type": "button", "bindings": ["key:ctrl+shift+z"]},
        {"name": "zoom", "type": "button", "bindings": ["key:z"]}]})");
    DeviceState d;
    frame(a, d, [](DeviceState& s) { s.keys.set(key("z")); });
    EXPECT_TRUE(a.state("zoom")->active);
    EXPECT_FALSE(a.state("undo")->active);
    frame(a, d, [](DeviceState& s) { s.keys.set(key("right_ctrl")); });
    EXPECT_TRUE(a.state("undo")->active);
    EXPECT_FALSE(a.state("zoom")->active) << "ctrl+z hides z";
    frame(a, d, [](DeviceState& s) { s.keys.set(key("left_shift")); });
    EXPECT_TRUE(a.state("redo")->active);
    EXPECT_FALSE(a.state("undo")->active) << "the chord with more modifiers wins";
}

constexpr const char* k_layered = R"({
    "contexts": [{"name": "camera", "active": true}, {"name": "tool"}, {"name": "modal", "exclusive": true}],
    "actions": [
      {"name": "camera.orbit", "type": "axis1d", "context": "camera",
       "bindings": [{"source": "mouse:delta_x", "with": "mouse:right"}]},
      {"name": "camera.drag", "type": "button", "context": "camera", "bindings": ["mouse:left"]},
      {"name": "tool.primary", "type": "button", "context": "tool", "bindings": ["mouse:left"]},
      {"name": "tool.cancel", "type": "button", "context": "tool", "bindings": ["key:escape"]},
      {"name": "modal.ok", "type": "button", "context": "modal", "bindings": ["key:enter"]}
    ]})";

TEST(InputActions, ContextPriorityConsumesControls) {
    Actions a = make(k_layered);
    DeviceState d;
    frame(a, d, [](DeviceState& s) { s.mouse.set(0); });
    EXPECT_TRUE(a.state("camera.drag")->active);
    EXPECT_FALSE(a.state("tool.primary")->active) << "the tool context is not on the stack";

    ASSERT_OK(a.push_context("tool"));
    frame(a, d, [](DeviceState&) {});
    EXPECT_TRUE(a.state("tool.primary")->active);
    EXPECT_FALSE(a.state("camera.drag")->active) << "the top context consumed the left button";
    EXPECT_TRUE(a.state("camera.drag")->ended);

    // Orbit reads other controls, so it still works under the tool.
    frame(a, d, [](DeviceState& s) {
        s.mouse.set(2);
        s.mouse_delta = {5.0F, 0.0F};
    });
    EXPECT_FLOAT_EQ(a.state("camera.orbit")->value.x, 5.0F);

    // An exclusive context hides everything below it.
    ASSERT_OK(a.push_context("modal"));
    frame(a, d, [](DeviceState& s) { s.keys.set(key("enter")); });
    EXPECT_TRUE(a.state("modal.ok")->active);
    EXPECT_FALSE(a.state("tool.primary")->active);
    EXPECT_FLOAT_EQ(a.state("camera.orbit")->value.x, 0.0F);

    EXPECT_TRUE(a.pop_context("modal"));
    EXPECT_FALSE(a.pop_context("modal"));
    EXPECT_TRUE(a.pop_context("tool"));
    frame(a, d, [](DeviceState&) {});
    EXPECT_TRUE(a.state("camera.drag")->active) << "back to the camera once the tool is gone";
    EXPECT_ERR(a.push_context("nope"));
}

TEST(InputActions, WithControlGatesTheBinding) {
    Actions a = make(k_layered);
    DeviceState d;
    frame(a, d, [](DeviceState& s) { s.mouse_delta = {7.0F, 0.0F}; });
    EXPECT_FLOAT_EQ(a.state("camera.orbit")->value.x, 0.0F) << "no right button held";
}

TEST(InputActions, Triggers) {
    Actions a = make(R"({"actions": [
        {"name": "tap", "type": "button", "trigger": {"tap": 0.2}, "bindings": ["key:t"]},
        {"name": "hold", "type": "button", "trigger": {"hold": 0.5}, "bindings": ["key:h"]},
        {"name": "dbl", "type": "button", "trigger": {"double_click": 0.3}, "bindings": ["mouse:left"]},
        {"name": "drag", "type": "button", "trigger": {"drag": 4}, "bindings": ["mouse:right"]},
        {"name": "rel", "type": "button", "trigger": "released", "bindings": ["key:r"]}]})");
    DeviceState d;
    const float dt = 0.1F;
    // Tap: a short press fires on release; a long one does not.
    frame(a, d, [](DeviceState& s) { s.keys.set(key("t")); }, dt);
    frame(a, d, [](DeviceState& s) { s.keys.reset(); }, dt);
    EXPECT_TRUE(a.state("tap")->started);
    frame(a, d, [](DeviceState& s) { s.keys.set(key("t")); }, dt);
    for (int i = 0; i < 4; ++i) frame(a, d, [](DeviceState&) {}, dt);
    frame(a, d, [](DeviceState& s) { s.keys.reset(); }, dt);
    EXPECT_FALSE(a.state("tap")->started);

    // Hold: active once held long enough.
    frame(a, d, [](DeviceState& s) { s.keys.set(key("h")); }, dt);
    int frames_to_hold = 0;
    while (!a.state("hold")->active && frames_to_hold < 20) {
        frame(a, d, [](DeviceState&) {}, dt);
        ++frames_to_hold;
    }
    EXPECT_EQ(frames_to_hold, 5) << "0.5 s at 0.1 s per frame";
    EXPECT_TRUE(a.state("hold")->started);
    frame(a, d, [](DeviceState& s) { s.keys.reset(); }, dt);

    // Double click within the gap fires on the second press only.
    frame(a, d, [](DeviceState& s) { s.mouse.set(0); }, dt);
    EXPECT_FALSE(a.state("dbl")->active);
    frame(a, d, [](DeviceState& s) { s.mouse.reset(0); }, dt);
    frame(a, d, [](DeviceState& s) { s.mouse.set(0); }, dt);
    EXPECT_TRUE(a.state("dbl")->started);
    frame(a, d, [](DeviceState& s) { s.mouse.reset(0); }, dt);

    // Drag: a click inside the dead zone is not a drag.
    frame(a, d, [](DeviceState& s) {
        s.mouse.set(2);
        s.mouse_position = {100.0F, 100.0F};
    });
    frame(a, d, [](DeviceState& s) { s.mouse_position = {102.0F, 101.0F}; });
    EXPECT_FALSE(a.state("drag")->active);
    frame(a, d, [](DeviceState& s) { s.mouse_position = {110.0F, 100.0F}; });
    EXPECT_TRUE(a.state("drag")->started);
    EXPECT_EQ(a.state("drag")->drag_origin, glm::vec2(100.0F, 100.0F));
    frame(a, d, [](DeviceState& s) { s.mouse.reset(2); });
    EXPECT_TRUE(a.state("drag")->ended);

    frame(a, d, [](DeviceState& s) { s.keys.set(key("r")); });
    EXPECT_FALSE(a.state("rel")->active);
    frame(a, d, [](DeviceState& s) { s.keys.reset(); });
    EXPECT_TRUE(a.state("rel")->active);
    frame(a, d, [](DeviceState&) {});
    EXPECT_FALSE(a.state("rel")->active);
}

TEST(InputActions, PointerBlockedByUiButCapturedDragsContinue) {
    Actions a = make(R"({"actions": [{"name": "grab", "type": "button", "bindings": ["mouse:left"]},
                                     {"name": "type", "type": "button", "bindings": ["key:w"]}]})");
    DeviceState d;
    frame(a, d, [](DeviceState& s) {
        s.pointer_blocked = true;
        s.mouse.set(0);
    });
    EXPECT_FALSE(a.state("grab")->active) << "a click on a UI panel never reaches the world";
    frame(a, d, [](DeviceState& s) {
        s.pointer_blocked = false;
        s.mouse.reset(0);
    });
    frame(a, d, [](DeviceState& s) { s.mouse.set(0); });
    EXPECT_TRUE(a.state("grab")->active);
    frame(a, d, [](DeviceState& s) { s.pointer_blocked = true; });
    EXPECT_TRUE(a.state("grab")->active) << "the drag keeps the pointer it started with";

    frame(a, d, [](DeviceState& s) {
        s.keyboard_blocked = true;
        s.keys.set(key("w"));
    });
    EXPECT_FALSE(a.state("type")->active) << "a text field owns the keyboard";
}

TEST(InputActions, EdgeZones) {
    Actions a = make(R"({"actions": [
        {"name": "edge", "type": "axis2d", "bindings": ["edge"]},
        {"name": "edge_win", "type": "axis2d", "bindings": [{"source": "edge", "windowed": true, "edge_px": 10}]}]})");
    DeviceState d;
    d.viewport_origin = {100.0F, 50.0F};
    d.viewport_size = {800.0F, 600.0F};
    frame(a, d, [](DeviceState& s) { s.mouse_position = {103.0F, 52.0F}; }); // top-left corner
    EXPECT_EQ(a.state("edge")->value, glm::vec2(0.0F)) << "edge scrolling is fullscreen-only by default";
    EXPECT_NEAR(a.state("edge_win")->value.x, -std::sqrt(0.5F), 1e-6F) << "corners are not faster";
    EXPECT_NEAR(a.state("edge_win")->value.y, std::sqrt(0.5F), 1e-6F);
    frame(a, d, [](DeviceState& s) {
        s.fullscreen = true;
        s.mouse_position = {895.0F, 400.0F}; // right edge
    });
    EXPECT_EQ(a.state("edge")->value, glm::vec2(1.0F, 0.0F));
    frame(a, d, [](DeviceState& s) { s.mouse_position = {500.0F, 400.0F}; });
    EXPECT_EQ(a.state("edge")->value, glm::vec2(0.0F));
    frame(a, d, [](DeviceState& s) { s.mouse_position = {1500.0F, 400.0F}; });
    EXPECT_EQ(a.state("edge")->value, glm::vec2(0.0F)) << "outside the viewport";
}

TEST(InputActions, ModifiersAndCameraRelative) {
    Actions a = make(R"({"actions": [
        {"name": "look", "type": "axis2d", "bindings": [{"source": "mouse:delta", "modifiers": [{"scale": [0.5, 2]}, {"invert": "y"}]}]},
        {"name": "move", "type": "axis2d", "modifiers": ["camera_relative"],
         "bindings": [{"composite": "2d", "up": "key:w"}]},
        {"name": "smooth", "type": "axis1d", "modifiers": [{"smoothing": 0.1}], "bindings": ["key:d"]}]})");
    DeviceState d;
    a.set_camera_yaw(glm::radians(90.0F)); // looking along +Z
    frame(a, d, [](DeviceState& s) {
        s.mouse_delta = {4.0F, 3.0F};
        s.keys.set(key("w"));
        s.keys.set(key("d"));
    });
    EXPECT_EQ(a.state("look")->value, glm::vec2(2.0F, -6.0F));
    EXPECT_NEAR(a.state("move")->value.x, 0.0F, 1e-6F);
    EXPECT_NEAR(a.state("move")->value.y, 1.0F, 1e-6F) << "forward is world +Z";
    // Smoothing: half the way after one half-life, still actuated at once.
    float elapsed = k_dt;
    while (elapsed < 0.1F - 1e-6F) {
        frame(a, d, [](DeviceState&) {});
        elapsed += k_dt;
    }
    EXPECT_TRUE(a.state("smooth")->actuated);
    EXPECT_GT(a.state("smooth")->value.x, 0.3F);
    EXPECT_LT(a.state("smooth")->value.x, 0.7F);
}

TEST(InputActions, SimSampleDeliversEachEdgeToOneStep) {
    Actions a;
    DeviceState d;
    // A tap of space during frames that run no simulation step...
    frame(a, d, [](DeviceState& s) { s.keys.set(key("space")); });
    frame(a, d, [](DeviceState& s) { s.keys.reset(); });
    // ...is seen by the next step, once.
    const auto step1 = a.take_sim_sample();
    const auto jump = static_cast<std::size_t>(a.find("jump"));
    EXPECT_TRUE(step1.states[jump].started);
    EXPECT_TRUE(step1.states[jump].ended);
    const auto step2 = a.take_sim_sample();
    EXPECT_FALSE(step2.states[jump].started);
    EXPECT_FALSE(step2.states[jump].ended);
}

TEST(InputActions, StepLatchMarksOneFrameTriggersActive) {
    Actions a = make(R"({"actions": [{"name": "p", "type": "button", "trigger": "pressed", "bindings": ["key:p"]}]})");
    DeviceState d;
    frame(a, d, [](DeviceState& s) { s.keys.set(key("p")); });
    frame(a, d, [](DeviceState&) {});
    EXPECT_FALSE(a.state("p")->active) << "pressed lasts one frame";
    const auto step = a.take_sim_sample();
    EXPECT_TRUE(step.states[0].active) << "but the step that missed it still sees it";
    EXPECT_TRUE(step.states[0].started);
}

TEST(InputActions, InjectionOverridesForFrames) {
    Actions a;
    DeviceState d;
    ASSERT_OK(a.inject("move_x", {0.5F, 0.0F}, 2));
    ASSERT_OK(a.inject("jump", {1.0F, 0.0F}, 1));
    frame(a, d, [](DeviceState&) {});
    EXPECT_FLOAT_EQ(a.state("move_x")->value.x, 0.5F);
    EXPECT_TRUE(a.state("jump")->started);
    frame(a, d, [](DeviceState&) {});
    EXPECT_FLOAT_EQ(a.state("move_x")->value.x, 0.5F);
    EXPECT_TRUE(a.state("jump")->ended);
    frame(a, d, [](DeviceState&) {});
    EXPECT_FLOAT_EQ(a.state("move_x")->value.x, 0.0F);
    EXPECT_ERR(a.inject("nope", {1.0F, 0.0F}, 1));
    EXPECT_ERR(a.inject("jump", {1.0F, 0.0F}, 0));
}

// Review #111: a one-frame trigger fired in one frame is active for exactly
// one take, even when a consumer takes twice before the next update (two
// fixed steps in one frame).
TEST(InputActions, OneFrameTriggerReachesExactlyOneTake) {
    Actions a = make(R"({"actions": [{"name": "kick", "type": "button", "trigger": "pressed", "bindings": ["key:k"]},
                                     {"name": "walk", "type": "button", "bindings": ["key:w"]}]})");
    DeviceState d;
    frame(a, d, [](DeviceState& s) {
        s.keys.set(key("k"));
        s.keys.set(key("w"));
    });
    const auto first = a.take_sim_sample();
    const auto second = a.take_sim_sample();
    EXPECT_TRUE(first.states[0].active);
    EXPECT_TRUE(first.states[0].started);
    EXPECT_FALSE(second.states[0].active) << "the press is consumed by the first step";
    EXPECT_FALSE(second.states[0].started);
    EXPECT_TRUE(second.states[1].active) << "a held down-trigger stays active";
    EXPECT_FALSE(second.states[1].started);

    // The same through a nested latch fed once and taken twice.
    engine::input::StepLatch latch;
    frame(a, d, [](DeviceState& s) { s.keys.reset(); });
    frame(a, d, [](DeviceState& s) { s.keys.set(key("k")); });
    latch.add(a.take_sim_sample());
    EXPECT_TRUE(latch.take(a.map()).states[0].active);
    EXPECT_FALSE(latch.take(a.map()).states[0].active);
}

// Review #111: a delta-driven orbit gated on a held button keeps the
// pointer on frames where the mouse does not move, so the drag continues
// across UI panels.
TEST(InputActions, PointerCaptureSurvivesFramesWithoutMotion) {
    Actions a = make(k_layered);
    DeviceState d;
    frame(a, d, [](DeviceState& s) {
        s.mouse.set(2);
        s.mouse_delta = {4.0F, 0.0F};
    });
    EXPECT_FLOAT_EQ(a.state("camera.orbit")->value.x, 4.0F);
    frame(a, d, [](DeviceState&) {}); // button held, no motion this frame
    frame(a, d, [](DeviceState& s) {
        s.pointer_blocked = true; // the pointer crossed onto a panel
        s.mouse_delta = {3.0F, 0.0F};
    });
    EXPECT_FLOAT_EQ(a.state("camera.orbit")->value.x, 3.0F) << "still captured";
    frame(a, d, [](DeviceState& s) {
        s.mouse.reset(2);
        s.mouse_delta = {3.0F, 0.0F};
    });
    frame(a, d, [](DeviceState& s) {
        s.mouse.set(2);
        s.mouse_delta = {3.0F, 0.0F};
    });
    EXPECT_FLOAT_EQ(a.state("camera.orbit")->value.x, 0.0F) << "a new press over the UI does not start";
}

// The same device sequence gives bit-identical samples (no clocks, no
// randomness), so a recorded input stream replays exactly.
TEST(InputActions, Deterministic) {
    const auto run = [] {
        Actions a = make(k_layered);
        DeviceState d;
        std::vector<engine::input::ActionSample> out;
        for (int f = 0; f < 240; ++f) {
            d.begin_frame();
            d.mouse.set(0, (f / 7) % 2 == 0);
            d.mouse.set(2, (f / 11) % 2 == 0);
            d.mouse_delta = {std::sin(static_cast<float>(f) * 0.3F) * 9.0F, 1.0F};
            d.mouse_position += d.mouse_delta;
            d.keys.set(key("escape"), f % 5 == 0);
            if (f == 60) (void)a.push_context("tool");
            if (f == 180) (void)a.pop_context("tool");
            a.update(d, 1.0F / (50.0F + static_cast<float>(f % 13)));
            out.push_back(f % 3 == 0 ? a.take_sim_sample() : a.frame());
        }
        return out;
    };
    EXPECT_EQ(run(), run());
}

} // namespace
} // namespace quat::test
