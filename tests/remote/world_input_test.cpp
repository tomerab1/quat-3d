// input.* and camera.rig.* world commands (G5.1 / G15.1).

#include <cmath>
#include <optional>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/input/actions.hpp"
#include "engine/input/device_state.hpp"
#include "engine/scene/camera_rig.hpp"
#include "world_fixture.hpp"

namespace quat::test {
namespace {

namespace rc = engine::remote::rpc_code;

class WorldInput : public WorldCommands {
protected:
    engine::input::Actions actions;
    int saves = 0;

    void SetUp() override {
        services.actions = &actions;
        services.save_input_bindings = [this]() -> std::expected<void, engine::core::Error> {
            ++saves;
            return {};
        };
        services.view_aspect = [] { return 2.0F; };
        register_commands();
    }
    void step() { actions.update(engine::input::DeviceState{}, 1.0F / 60.0F); }
};

TEST_F(WorldInput, ListsActionsAndContexts) {
    const Json out = ok("input.actions");
    ASSERT_TRUE(out["actions"].is_array());
    EXPECT_EQ(out["stack"], Json::array({"default"}));
    bool jump = false;
    for (const Json& a : out["actions"]) {
        if (a["name"] == "jump") {
            jump = true;
            EXPECT_EQ(a["type"], "button");
            EXPECT_EQ(a["bindings"][0], "key:space");
            EXPECT_EQ(a["trigger"], "down");
        }
    }
    EXPECT_TRUE(jump);
    EXPECT_EQ(ok("input.actions", {{"action", "move_x"}})["actions"].size(), 1U);
    EXPECT_NE(err("input.actions", {{"action", "nope"}}, rc::invalid_params).find("nope"), std::string::npos);
}

TEST_F(WorldInput, InjectDrivesTheActionForFrames) {
    ok("input.inject", {{"action", "jump"}, {"value", true}, {"frames", 2}});
    ok("input.inject", {{"action", "move_x"}, {"value", -0.5}});
    step();
    const Json jump = ok("input.actions", {{"action", "jump"}})["actions"][0];
    EXPECT_TRUE(jump["active"].get<bool>());
    EXPECT_FLOAT_EQ(actions.state("move_x")->value.x, -0.5F);
    step();
    EXPECT_TRUE(actions.state("jump")->active);
    EXPECT_FLOAT_EQ(actions.state("move_x")->value.x, 0.0F) << "one frame by default";
    ok("input.inject", {{"action", "jump"}, {"clear", true}});
    step();
    EXPECT_FALSE(actions.state("jump")->active);

    EXPECT_NE(err("input.inject", {{"action", "jump"}}, rc::invalid_params).find("value"), std::string::npos);
    EXPECT_NE(err("input.inject", {{"action", "warp"}, {"value", 1}}, rc::invalid_params).find("known:"),
              std::string::npos);
    err("input.inject", {{"action", "jump"}, {"value", 1}, {"frames", 0}}, rc::invalid_params);
    EXPECT_NE(err("input.inject", {{"action", "move_x"}, {"value", {1, 0}}}, rc::invalid_params).find("axis1d"),
              std::string::npos)
        << "a 2D value on a 1D action is refused, not truncated";
}

TEST_F(WorldInput, BindRebindsSavesAndResets) {
    const Json out = ok("input.bind", {{"action", "jump"}, {"bindings", {"key:j", "gamepad:north"}}});
    EXPECT_EQ(out["bindings"], Json::array({"key:j", "gamepad:north"}));
    EXPECT_TRUE(out["saved"].get<bool>());
    EXPECT_EQ(saves, 1);
    ok("input.bind", {{"action", "jump"}, {"bindings", nullptr}, {"save", false}});
    EXPECT_EQ(saves, 1);
    EXPECT_TRUE(actions.overrides_json()["bindings"].empty());
    EXPECT_NE(err("input.bind", {{"action", "jump"}, {"bindings", {"key:hyperspace"}}}, rc::invalid_params)
                  .find("hyperspace"),
              std::string::npos);
}

TEST_F(WorldInput, ContextPushPop) {
    err("input.context", {{"push", "camera"}}, rc::invalid_params);
    err("input.context", Json::object(), rc::invalid_params);
    EXPECT_NE(err("input.context", {{"pop", "default"}, {"push", "default"}}, rc::invalid_params).find("exactly one"),
              std::string::npos);
    EXPECT_EQ(ok("input.context", {{"pop", "default"}})["stack"], Json::array());
    EXPECT_EQ(ok("input.context", {{"push", "default"}})["stack"], Json::array({"default"}));
}

class WorldInputBare : public WorldCommands {
protected:
    void SetUp() override { register_commands(); }
};

TEST_F(WorldInputBare, UnavailableWithoutTheService) {
    err("input.actions", Json::object(), rc::unavailable);
    err("input.inject", {{"action", "jump"}, {"value", true}}, rc::unavailable);
}

// ---- camera.rig.* ----------------------------------------------------------------

TEST_F(WorldInput, RigGetSetFrame) {
    EXPECT_NE(err("camera.rig.get", Json::object(), rc::not_found).find("camera_rig"), std::string::npos);

    const entt::entity rig = scene.create_entity("rig");
    reg().emplace<engine::scene::Camera>(rig);
    reg().emplace<engine::scene::CameraRig>(rig);
    const entt::entity box = scene.create_entity("box");
    reg().emplace<engine::scene::Primitive>(box);
    reg().get<engine::scene::Transform>(box).local = glm::translate(glm::mat4(1.0F), glm::vec3(50.0F, 0.0F, 20.0F));

    Json out = ok("camera.rig.get");
    EXPECT_EQ(entity(out["entity"]), rig) << "found without naming it";
    EXPECT_TRUE(out.contains("camera_rig"));

    out = ok("camera.rig.set", {{"focus", {10, 0, 5}}, {"yaw_deg", 90}, {"distance", 30}, {"immediate", true}});
    EXPECT_EQ(out["camera_rig"]["focus"], Json::array({10.0, 0.0, 5.0}));
    ASSERT_TRUE(out["camera_rig"].contains("current")) << "immediate snaps the damped pose";
    EXPECT_NEAR(out["camera_rig"]["current"]["distance"].get<float>(), 30.0F, 1e-3F);
    const glm::vec3 eye(reg().get<engine::scene::Transform>(rig).local[3]);
    const glm::vec3 want = engine::scene::rig_eye({10, 0, 5}, 90.0F,
                                                  engine::scene::rig_pitch_for_distance(reg().get<engine::scene::CameraRig>(rig), 30.0F), 30.0F);
    EXPECT_NEAR(glm::length(eye - want), 0.0F, 1e-3F) << "the entity is placed at once";

    EXPECT_NE(err("camera.rig.set", {{"pitch_deg", 200}}, rc::invalid_params).find("pitch_deg"), std::string::npos);
    EXPECT_NE(err("camera.rig.set", {{"zoom", 2}}, rc::invalid_params).find("unknown field"), std::string::npos);
    err("camera.rig.set", {{"immediate", 1}}, rc::invalid_params);

    out = ok("camera.rig.frame", {{"target", "box"}, {"immediate", true}});
    const auto focus = out["camera_rig"]["focus"];
    EXPECT_NEAR(focus[0].get<float>(), 50.0F, 1e-3F);
    EXPECT_NEAR(focus[2].get<float>(), 20.0F, 1e-3F);
    EXPECT_FLOAT_EQ(out["camera_rig"]["yaw_deg"].get<float>(), 90.0F) << "framing keeps the heading";
    err("camera.rig.frame", {{"target", "missing"}}, rc::not_found);
}

} // namespace
} // namespace quat::test
