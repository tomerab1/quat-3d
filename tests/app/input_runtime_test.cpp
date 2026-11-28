// InputRuntime (G5.1 + G15.1 app layer): the project action map merged over
// the defaults, the user's rebinding file, and camera rigs taking input only
// while the game view is live.

#include <fstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "app/input_runtime.hpp"
#include "engine/scene/camera_rig.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace quat::test {
namespace {

constexpr const char* k_project_map = R"({
    "contexts": [{"name": "camera", "active": true}],
    "actions": [
      {"name": "camera.pan", "type": "axis2d", "context": "camera",
       "bindings": [{"composite": "2d", "up": "key:w", "down": "key:s", "left": "key:a", "right": "key:d"}]}
    ]})";

struct InputRuntimeTest : ::testing::Test {
    TempDir dir{"input-runtime"};
    app::InputRuntime runtime;
    engine::scene::Scene scene;
    entt::entity rig = entt::null;

    void SetUp() override {
        std::filesystem::create_directories(dir.path() / "assets" / "input");
        std::ofstream(dir.path() / "assets" / "input" / "actions.json") << k_project_map;
        std::ofstream(dir.path() / "user.json") << R"({"version": 1, "bindings": {"jump": ["key:j"]}})";
        runtime.init(dir.path(), dir.path() / "user.json");

        rig = scene.create_entity("rig");
        auto& r = scene.registry().emplace<engine::scene::CameraRig>(rig);
        r.focus = glm::vec3(0.0F);
        scene.registry().emplace<engine::scene::CameraRigInput>(rig).pan = "camera.pan";
    }
    void frame(bool rig_input, bool hold_w) {
        engine::input::DeviceState d;
        d.viewport_size = {800.0F, 600.0F};
        if (hold_w) d.keys.set(*engine::input::key_from_name("w"));
        runtime.update(d, 0.1F, 0.0F);
        runtime.update_rigs(scene, {}, nullptr, rig_input, 0.1F);
    }
};

TEST_F(InputRuntimeTest, ProjectMapMergesOverTheDefaults) {
    const auto& map = runtime.actions().map();
    EXPECT_GE(map.find_action("camera.pan"), 0);
    EXPECT_GE(map.find_action("move_x"), 0) << "G7's names stay";
    EXPECT_TRUE(runtime.actions().context_active("camera"));
}

TEST_F(InputRuntimeTest, UserOverridesApplyAndSave) {
    EXPECT_EQ(runtime.actions().overrides_json()["bindings"]["jump"], nlohmann::json::array({"key:j"}));
    ASSERT_OK(runtime.save_overrides());
    app::InputRuntime again;
    again.init(dir.path(), dir.path() / "user.json");
    EXPECT_EQ(again.actions().overrides_json(), runtime.actions().overrides_json());
}

TEST_F(InputRuntimeTest, RigsTakeInputOnlyWhenTheGameViewIsLive) {
    frame(false, true);
    EXPECT_EQ(scene.registry().get<engine::scene::CameraRig>(rig).focus, glm::vec3(0.0F))
        << "the editor's fly camera owns WASD";
    frame(true, true);
    EXPECT_GT(glm::length(scene.registry().get<engine::scene::CameraRig>(rig).focus), 1.0F);
    const glm::mat4 local = scene.registry().get<engine::scene::Transform>(rig).local;
    EXPECT_NE(local, glm::mat4(1.0F)) << "the rig drives the transform";
}

} // namespace
} // namespace quat::test
