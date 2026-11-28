// camera_rig / camera_rig_input through the component codec (G15.1): round
// trip, the read-only `current` pose, validation.

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/camera_rig.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "support/expect_ok.hpp"

namespace quat::test {
namespace {

namespace s = engine::scene;
using nlohmann::json;

TEST(CameraRigCodec, RoundTripsAndIgnoresTheCurrentPose) {
    entt::registry r;
    const entt::entity e = r.create();
    r.emplace<s::Transform>(e);
    ASSERT_OK(s::apply_component(r, e, "camera_rig",
                                 {{"focus", {1, 2, 3}}, {"yaw_deg", 45}, {"distance", 80},
                                  {"pitch_near_deg", -35}, {"yaw_step_deg", 90},
                                  {"bounds", {{"shape", "rect"}, {"min", {-50, -40}}, {"max", {50, 40}}, {"margin", 20}}}},
                                 true));
    ASSERT_OK(s::apply_component(r, e, "camera_rig_input",
                                 {{"pan", "camera.pan"}, {"zoom", "camera.zoom"}, {"zoom_step", 1.2}}, true));
    s::camera_rig_snap(r.get<s::CameraRig>(e));

    const json first = s::encode_components(r, e, {});
    ASSERT_TRUE(first["camera_rig"].contains("current")) << "the damped pose is reported";
    EXPECT_EQ(first["camera_rig"]["bounds"]["shape"], "rect");
    EXPECT_EQ(first["camera_rig_input"]["pan"], "camera.pan");

    const entt::entity copy = r.create();
    for (const auto& [name, value] : first.items()) {
        ASSERT_OK(s::apply_component(r, copy, name, value, true)) << name;
    }
    json a = first;
    json b = s::encode_components(r, copy, {});
    a["camera_rig"].erase("current");
    EXPECT_FALSE(b["camera_rig"].contains("current")) << "a decoded rig has not run yet";
    EXPECT_EQ(a, b);
}

TEST(CameraRigCodec, RejectsBadValues) {
    entt::registry r;
    const entt::entity e = r.create();
    EXPECT_ERR(s::apply_component(r, e, "camera_rig", {{"pitch_deg", 120}}, true));
    EXPECT_ERR(s::apply_component(r, e, "camera_rig", {{"min_distance", 50}, {"max_distance", 10}}, true));
    EXPECT_ERR(s::apply_component(r, e, "camera_rig", {{"bounds", {{"shape", "hexagon"}}}}, true));
    EXPECT_ERR(s::apply_component(r, e, "camera_rig", {{"bounds", {{"min", {1, 2, 3}}}}}, true));
    EXPECT_ERR(s::apply_component(r, e, "camera_rig", {{"zoom", 3}}, true)) << "unknown field";
    EXPECT_ERR(s::apply_component(r, e, "camera_rig_input", {{"zoom_step", 0.5}}, true));
    EXPECT_ERR(s::apply_component(r, e, "camera_rig", {{"yaw_step_deg", 50}}, true)) << "must divide 360";
    EXPECT_FALSE(r.all_of<s::CameraRig>(e)) << "a failed decode adds nothing";
}

} // namespace
} // namespace quat::test
