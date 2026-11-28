// Remote character driving (A10.1 character.move) in character_system:
// walking to a target point stops there, a timed walk stops when its time
// runs out, and a jump request is honoured only from the ground.

#include <algorithm>
#include <optional>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
using engine::physics::PhysicsWorld;

constexpr float k_dt = 1.0F / 60.0F;

class CharacterMove : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));
        PhysicsWorld::BodyParams floor;
        floor.shape = world->create_box(glm::vec3(50.0F, 1.0F, 50.0F));
        floor.position = glm::vec3(0.0F, -1.0F, 0.0F);
        floor.motion = engine::physics::Motion::static_body;
        floor.layer = engine::physics::Layer::static_body;
        ASSERT_NE(world->add_body(floor), PhysicsWorld::invalid_body);
        hero = registry.create();
        auto& t = registry.emplace<s::Transform>(hero);
        t.local = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 1.0F, 0.0F));
        t.world = t.local;
        registry.emplace<s::CharacterController>(hero); // 4 m/s
        frames(60);                                      // land
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) s::character_system(registry, *world, k_dt);
    }
    [[nodiscard]] glm::vec3 pos() const {
        return glm::vec3(registry.get<s::Transform>(hero).world[3]);
    }
    [[nodiscard]] s::CharacterController& cc() { return registry.get<s::CharacterController>(hero); }

    std::optional<PhysicsWorld> world;
    entt::registry              registry;
    entt::entity                hero = entt::null;
};

TEST_F(CharacterMove, WalksToATargetAndStopsThere) {
    cc().move_target = glm::vec3(3.0F, 0.0F, 4.0F); // 5 m away
    cc().has_move_target = true;
    frames(60); // 1 s at 4 m/s: on the way
    EXPECT_GT(glm::length(glm::vec2(pos().x, pos().z)), 3.0F);
    frames(60);
    EXPECT_NEAR(pos().x, 3.0F, 0.05F);
    EXPECT_NEAR(pos().z, 4.0F, 0.05F);
    EXPECT_FALSE(cc().has_move_target) << "arrival should end the walk";
    EXPECT_EQ(cc().move, glm::vec3(0.0F));
    frames(30);
    EXPECT_NEAR(pos().x, 3.0F, 0.05F) << "kept walking after arriving";
}

TEST_F(CharacterMove, ATimedWalkStops) {
    cc().move = glm::vec3(1.0F, 0.0F, 0.0F);
    cc().move_time_left = 0.5F;
    frames(90);
    EXPECT_NEAR(pos().x, 2.0F, 0.15F); // 4 m/s for 0.5 s
    EXPECT_EQ(cc().move, glm::vec3(0.0F));
    EXPECT_LT(cc().move_time_left, 0.0F);
}

TEST_F(CharacterMove, JumpsOnlyFromTheGround) {
    const float ground = pos().y;
    cc().jump_speed = 5.0F;
    frames(10);
    EXPECT_FALSE(cc().on_ground) << "did not take off";
    EXPECT_EQ(cc().jump_speed, 0.0F) << "the request is consumed";

    // A request made in the air is dropped, not saved for the landing.
    cc().jump_speed = 5.0F;
    float peak = pos().y;
    for (int i = 0; i < 150; ++i) {
        frames(1);
        peak = std::max(peak, pos().y);
    }
    // One jump: v^2 / 2g = 25 / 19.62 ~ 1.27 m.
    EXPECT_NEAR(peak - ground, 1.27F, 0.15F);
    EXPECT_NEAR(pos().y, ground, 0.05F) << "jumped again after landing";
    EXPECT_TRUE(cc().on_ground);
}

} // namespace
