// Physics components in the JSON codec (A10.1): rigidbody surface / motion
// tuning (authoring data saved with scenes) and the joint component.

#include <string_view>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "support/expect_ok.hpp"

namespace {

using json = nlohmann::json;
namespace s = engine::scene;

class PhysicsCodec : public ::testing::Test {
protected:
    entt::registry r;
    entt::entity   e = r.create();

    [[nodiscard]] auto apply(std::string_view name, const json& v, bool strict = true) {
        return s::apply_component(r, e, name, v, strict);
    }
};

TEST_F(PhysicsCodec, RigidBodyTuningDefaultsToJoltsValues) {
    ASSERT_OK(apply("rigidbody", json::object()));
    const json j = s::encode_components(r, e)["rigidbody"];
    EXPECT_NEAR(j["friction"].get<float>(), 0.2F, 1e-6F);
    EXPECT_NEAR(j["restitution"].get<float>(), 0.0F, 1e-6F);
    EXPECT_NEAR(j["gravity_factor"].get<float>(), 1.0F, 1e-6F);
    EXPECT_NEAR(j["linear_damping"].get<float>(), 0.05F, 1e-6F);
    EXPECT_NEAR(j["angular_damping"].get<float>(), 0.05F, 1e-6F);
}

TEST_F(PhysicsCodec, RigidBodyTuningIsRangeChecked) {
    EXPECT_FALSE(apply("rigidbody", {{"friction", -1}}));
    EXPECT_FALSE(apply("rigidbody", {{"restitution", 1.5}}));
    EXPECT_FALSE(apply("rigidbody", {{"gravity_factor", 11}}));
    EXPECT_FALSE(apply("rigidbody", {{"linear_damping", "lots"}}));
    const auto bad = apply("rigidbody", {{"restitution", 2}});
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error().message.find("rigidbody.restitution"), std::string::npos);
    ASSERT_OK(apply("rigidbody", {{"gravity_factor", -2}, {"restitution", 1}}));
    EXPECT_FLOAT_EQ(r.get<s::RigidBody>(e).gravity_factor, -2.0F);
}

TEST_F(PhysicsCodec, JointRoundTripsWithItsTarget) {
    const entt::entity door_frame = r.create();
    r.emplace<s::Name>(door_frame, "Frame");
    ASSERT_OK(apply("joint", {{"type", "hinge"},
                              {"target", "Frame"},
                              {"anchor", {-0.5, 0, 0}},
                              {"axis", {0, 1, 0}},
                              {"limits", {-90, 10}},
                              {"motor", {{"mode", "velocity"}, {"target", 30}, {"max_force", 50}}}}));
    const s::Joint& j = r.get<s::Joint>(e);
    EXPECT_EQ(j.type, s::JointType::hinge);
    EXPECT_EQ(j.target, door_frame);
    EXPECT_TRUE(j.limits_enabled);
    EXPECT_EQ(j.motor, s::JointMotorMode::velocity);

    const json encoded = s::encode_components(r, e)["joint"];
    EXPECT_EQ(encoded["target"], entt::to_integral(door_frame));
    EXPECT_EQ(encoded["limits"], json::array({-90.0, 10.0}));
    EXPECT_FALSE(encoded["active"].get<bool>());
    const entt::entity copy = r.create();
    ASSERT_OK(s::apply_component(r, copy, "joint", encoded, true));
    EXPECT_EQ(s::encode_components(r, copy)["joint"], encoded);

    // null clears limits, motor and target (the world).
    ASSERT_OK(apply("joint", {{"limits", nullptr}, {"motor", nullptr}, {"target", nullptr}}));
    EXPECT_FALSE(r.get<s::Joint>(e).limits_enabled);
    EXPECT_EQ(r.get<s::Joint>(e).motor, s::JointMotorMode::off);
    EXPECT_EQ(r.get<s::Joint>(e).target, entt::entity{entt::null});
}

TEST_F(PhysicsCodec, JointRulesArePerType) {
    const auto expect_error = [&](const json& v, std::string_view fragment) {
        const auto result = apply("joint", v);
        ASSERT_FALSE(result) << v.dump();
        EXPECT_NE(result.error().message.find(fragment), std::string::npos)
            << result.error().message;
    };
    expect_error({{"type", "fixed"}, {"limits", {0, 1}}}, "no limits");
    expect_error({{"type", "point"}, {"motor", {{"mode", "velocity"}}}}, "only hinge and slider");
    expect_error({{"type", "hinge"}, {"limits", {10, 90}}}, "min in [-180, 0]");
    expect_error({{"type", "slider"}, {"limits", {0.5, 2}}}, "min <= 0 <= max");
    expect_error({{"type", "distance"}, {"limits", {2, 1}}}, "0 <= min <= max");
    expect_error({{"type", "hinge"}, {"axis", {0, 0, 0}}}, "non-zero");
    expect_error({{"type", "hinge"}, {"target", e}}, "its own entity");
    expect_error({{"type", "hinge"}, {"target", "Nobody"}}, "no entity named");
    expect_error({{"type", "hinge"}, {"target", 123456}}, "no entity with id");
    expect_error({{"type", "spring"}}, "\"hinge\"");
    expect_error({{"type", "hinge"}, {"motor", {{"mode", "velocity"}, {"speed", 3}}}}, "speed");
    EXPECT_FALSE(r.all_of<s::Joint>(e)) << "a failed decode added the component";
}

TEST_F(PhysicsCodec, EditingAJointResetsOnlyItsConstraint) {
    ASSERT_OK(apply("rigidbody", json::object()));
    ASSERT_OK(apply("joint", {{"type", "point"}}));
    r.get<s::RigidBody>(e).body = 5; // as if built
    r.get<s::Joint>(e).constraint = 9;
    ASSERT_OK(apply("joint", {{"anchor", {0, 1, 0}}}));
    EXPECT_EQ(r.get<s::Joint>(e).constraint, 0xFFFFFFFFU);
    EXPECT_EQ(r.get<s::RigidBody>(e).body, 5U) << "a joint edit must not rebuild the body";
    EXPECT_TRUE(s::remove_component(r, e, "joint"));
    EXPECT_FALSE(r.all_of<s::Joint>(e));
}

TEST_F(PhysicsCodec, OlderFilesWithoutTuningKeepDefaults) {
    ASSERT_OK(apply("rigidbody", {{"motion", 1}, {"mass", 3}}, false));
    const auto& rb = r.get<s::RigidBody>(e);
    EXPECT_FLOAT_EQ(rb.friction, 0.2F);
    EXPECT_FLOAT_EQ(rb.gravity_factor, 1.0F);
}

} // namespace
