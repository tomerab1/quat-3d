// The sdk-consumer module's tests: GoogleTest from the SDK's bundled deps/,
// engine types from the installed headers only.

#include <string>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/remote/rpc_server.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "engine/version.hpp"
#include "spinner.hpp"

namespace {

using consumer::Spinner;

TEST(SdkConsumer, SdkVersionMatchesHeaders) {
    EXPECT_STREQ(quat3d::version_string(), QUAT3D_VERSION_STRING);
    EXPECT_EQ(quat3d::version().minor, QUAT3D_VERSION_MINOR);
}

TEST(SdkConsumer, SystemRotatesAndKeepsTranslation) {
    engine::scene::Scene scene;
    const entt::entity e = scene.create_entity("s");
    scene.registry().get<engine::scene::Transform>(e).local[3] = glm::vec4(1.0F, 2.0F, 3.0F, 1.0F);
    scene.registry().emplace<Spinner>(e, Spinner{90.0F, 0.0F});

    consumer::spinner_system(scene.registry(), 1.0F);
    scene.tick(0.0F);

    EXPECT_FLOAT_EQ(scene.registry().get<Spinner>(e).angle_degrees, 90.0F);
    const glm::mat4& world = scene.registry().get<engine::scene::Transform>(e).world;
    EXPECT_NEAR(world[0].z, -1.0F, 1e-5F); // +X rotated 90 degrees about +Y
    EXPECT_FLOAT_EQ(world[3].x, 1.0F);
    EXPECT_FLOAT_EQ(world[3].z, 3.0F);
}

TEST(SdkConsumer, RemoteMethodListsSpinnersAndValidatesParams) {
    engine::scene::Scene scene;
    scene.registry().emplace<Spinner>(scene.create_entity("a"), Spinner{10.0F, 5.0F});
    engine::remote::RpcServer server;
    consumer::register_spinner_methods(server, scene);

    auto ok = server.call("consumer.spinners", engine::remote::Json::object());
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ((*ok)["count"], 1);
    auto bad = server.call("consumer.spinners", engine::remote::Json{{"x", 1}});
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, engine::remote::rpc_code::invalid_params);
}

TEST(SdkConsumer, SaveChunkRoundTripsAndRejectsBadInput) {
    engine::scene::Scene scene;
    const entt::entity e = scene.create_entity("wheel");
    scene.registry().emplace<Spinner>(e, Spinner{30.0F, 120.0F});
    const nlohmann::json chunk = consumer::save_spinners(scene.registry());

    scene.registry().remove<Spinner>(e);
    auto restored = consumer::load_spinners(scene.registry(), chunk);
    ASSERT_TRUE(restored.has_value()) << restored.error().message;
    EXPECT_EQ(*restored, 1U);
    EXPECT_FLOAT_EQ(scene.registry().get<Spinner>(e).angle_degrees, 120.0F);

    nlohmann::json bad = chunk;
    bad["spinners"]["wheel"]["angle_degrees"] = 999.0;
    auto refused = consumer::load_spinners(scene.registry(), bad);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message.find("angle_degrees"), std::string::npos)
        << refused.error().message;
    EXPECT_FALSE(consumer::load_spinners(scene.registry(), nlohmann::json::array()).has_value());
}

// The engine's own components reach a game through the installed codec.
TEST(SdkConsumer, EngineComponentCodecIsAvailable) {
    EXPECT_NE(engine::scene::find_component_info("transform"), nullptr);
}

} // namespace
