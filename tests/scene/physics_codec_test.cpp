// Physics components in the JSON codec (A10.1): rigidbody surface / motion
// tuning (authoring data saved with scenes).

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

TEST_F(PhysicsCodec, OlderFilesWithoutTuningKeepDefaults) {
    ASSERT_OK(apply("rigidbody", {{"motion", 1}, {"mass", 3}}, false));
    const auto& rb = r.get<s::RigidBody>(e);
    EXPECT_FLOAT_EQ(rb.friction, 0.2F);
    EXPECT_FLOAT_EQ(rb.gravity_factor, 1.0F);
}

} // namespace
