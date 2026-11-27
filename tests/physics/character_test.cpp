// Character controllers through the ECS (character_system): a capsule falls
// and grounds on a floor, walks along `move`, and its CharacterVirtual is
// removed with its entity (S1 audit B6).

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

class CharacterSystem : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));

        // Static floor (top at y=0), added directly to the world.
        PhysicsWorld::BodyParams floor;
        floor.shape = world->create_box(glm::vec3(50.0F, 1.0F, 50.0F));
        floor.position = glm::vec3(0.0F, -1.0F, 0.0F);
        floor.motion = engine::physics::Motion::static_body;
        floor.layer = engine::physics::Layer::static_body;
        ASSERT_NE(world->add_body(floor), PhysicsWorld::invalid_body);

        // The controller spawns at Transform.world; no transform system runs
        // on this bare registry, so world is set with local.
        character = registry.create();
        auto& t = registry.emplace<s::Transform>(character);
        t.local = glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 5.0F, 0.0F));
        t.world = t.local;
        registry.emplace<s::CharacterController>(character);
    }

    void step(int frames) {
        for (int i = 0; i < frames; ++i) s::character_system(registry, *world, k_dt);
    }

    [[nodiscard]] glm::vec3 local_position() const {
        return glm::vec3(registry.get<s::Transform>(character).local[3]);
    }

    std::optional<PhysicsWorld> world;
    entt::registry              registry;
    entt::entity                character = entt::null;
};

TEST_F(CharacterSystem, FallsAndLandsOnTheFloor) {
    step(240); // 4 s
    const float landed_y = local_position().y;
    EXPECT_LE(landed_y, 4.0F) << "did not fall";
    EXPECT_GE(landed_y, 0.0F) << "sank through the floor";
    EXPECT_TRUE(registry.get<s::CharacterController>(character).on_ground)
        << "not grounded after landing";
}

TEST_F(CharacterSystem, WalksAlongItsMoveDirection) {
    step(240); // land first
    const float start_x = local_position().x;
    registry.get<s::CharacterController>(character).move = glm::vec3(1.0F, 0.0F, 0.0F);
    step(60); // 1 s
    EXPECT_GE(local_position().x - start_x, 1.0F) << "did not walk forward";
}

TEST_F(CharacterSystem, DestroyingTheEntityRemovesItsCharacter) {
    step(1);
    ASSERT_EQ(world->owned_characters().size(), 1U);
    registry.destroy(character);
    step(1);
    EXPECT_TRUE(world->owned_characters().empty()) << "orphaned character kept";
}

} // namespace
