// Rebinding and its per-user file (G5.1): overrides hold only changed
// actions, survive a map reload, round-trip through the file, and a bad file
// changes nothing.

#include <fstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/input/actions.hpp"
#include "engine/input/device_state.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace quat::test {
namespace {

using engine::input::Actions;
using engine::input::DeviceState;
using nlohmann::json;

[[nodiscard]] engine::input::Binding binding(const char* text) {
    auto b = engine::input::parse_binding(json(text), "b");
    EXPECT_TRUE(b.has_value());
    return b ? *b : engine::input::Binding{};
}

TEST(InputRebinding, RebindChangesWhatFires) {
    Actions a;
    ASSERT_OK(a.rebind("jump", {binding("key:j")}));
    DeviceState d;
    d.keys.set(*engine::input::key_from_name("space"));
    a.update(d, 0.016F);
    EXPECT_FALSE(a.state("jump")->active);
    d.begin_frame();
    d.keys.set(*engine::input::key_from_name("j"));
    a.update(d, 0.016F);
    EXPECT_TRUE(a.state("jump")->active);
    EXPECT_ERR(a.rebind("nope", {}));
}

TEST(InputRebinding, OverridesListOnlyChangedActions) {
    Actions a;
    EXPECT_TRUE(a.overrides_json()["bindings"].empty());
    ASSERT_OK(a.rebind("jump", {binding("key:j"), binding("gamepad:north")}));
    const json o = a.overrides_json();
    ASSERT_EQ(o["bindings"].size(), 1U);
    EXPECT_EQ(o["bindings"]["jump"], json::array({"key:j", "gamepad:north"}));
    ASSERT_OK(a.reset_bindings("jump"));
    EXPECT_TRUE(a.overrides_json()["bindings"].empty());
}

TEST(InputRebinding, FileRoundTripAndBadFilesChangeNothing) {
    TempDir dir("input-overrides");
    const auto path = dir.path() / "user" / "input.json";

    Actions a;
    ASSERT_OK(engine::input::load_overrides_file(a, path)) << "a missing file is not an error";
    ASSERT_OK(a.rebind("fire", {binding("mouse:x1")}));
    ASSERT_OK(engine::input::save_overrides_file(a, path));

    Actions b;
    ASSERT_OK(engine::input::load_overrides_file(b, path));
    EXPECT_EQ(b.overrides_json(), a.overrides_json());

    // Rebinds survive a map reload for actions the new map still has.
    b.set_map(engine::input::default_action_map());
    EXPECT_EQ(b.overrides_json(), a.overrides_json());

    {
        std::ofstream bad(path, std::ios::trunc);
        bad << R"({"version": 1, "bindings": {"jump": ["key:j"], "fire": ["key:hyperspace"]}})";
    }
    Actions c;
    auto err = engine::input::load_overrides_file(c, path);
    ASSERT_ERR(err);
    EXPECT_NE(err.error().message.find("fire[0]"), std::string::npos) << err.error().message;
    EXPECT_TRUE(c.overrides_json()["bindings"].empty()) << "validated whole before applying";

    {
        std::ofstream junk(path, std::ios::trunc);
        junk << "{not json";
    }
    EXPECT_ERR(engine::input::load_overrides_file(c, path));
}

// Review #111: a rebind survives a map reload only onto an action of the
// same type.
TEST(InputRebinding, MapReloadDropsRebindsWhoseTypeChanged) {
    Actions a;
    ASSERT_OK(a.rebind("jump", {binding("key:j")}));
    ASSERT_OK(a.rebind("move_x", {binding("gamepad:right_x")}));
    engine::input::ActionMap changed = engine::input::default_action_map();
    changed.actions[static_cast<std::size_t>(changed.find_action("jump"))].type = engine::input::ActionType::axis1d;
    a.set_map(changed);
    const json o = a.overrides_json()["bindings"];
    EXPECT_FALSE(o.contains("jump")) << "the type changed";
    EXPECT_EQ(o["move_x"], json::array({"gamepad:right_x"}));
}

} // namespace
} // namespace quat::test
