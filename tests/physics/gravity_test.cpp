// One gravity setting (A10.1): the scene's PhysicsSettings drive Jolt and
// the character controllers alike, a change wakes resting bodies, and the
// settings codec validates its input.

#include <optional>
#include <utility>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/physics_settings.hpp"
#include "engine/scene/scene.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace s = engine::scene;
using engine::physics::PhysicsWorld;

constexpr float k_dt = PhysicsWorld::fixed_timestep;

class Gravity : public ::testing::Test {
protected:
    void SetUp() override {
        auto w = PhysicsWorld::create();
        ASSERT_OK(w);
        world.emplace(std::move(*w));
    }

    entt::entity place(const glm::vec3& pos) {
        const entt::entity e = registry.create();
        auto& t = registry.emplace<s::Transform>(e);
        t.local = glm::translate(glm::mat4(1.0F), pos);
        t.world = t.local;
        return e;
    }
    entt::entity add_floor() {
        const entt::entity f = place(glm::vec3(0.0F, -1.0F, 0.0F));
        registry.emplace<s::Collider>(f, s::ColliderShape::box, glm::vec3(50.0F, 1.0F, 50.0F),
                                      glm::vec3(0.0F));
        registry.emplace<s::RigidBody>(f, s::BodyMotion::static_body, 1.0F,
                                       PhysicsWorld::invalid_body);
        return f;
    }
    entt::entity add_ball(const glm::vec3& pos) {
        const entt::entity b = place(pos);
        registry.emplace<s::Collider>(b, s::ColliderShape::sphere, glm::vec3(0.5F),
                                      glm::vec3(0.0F));
        auto& rb = registry.emplace<s::RigidBody>(b);
        rb.linear_damping = 0.0F;
        return b;
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            s::character_system(registry, *world, k_dt);
            s::physics_system(registry, *world, k_dt);
        }
    }
    [[nodiscard]] glm::vec3 velocity(entt::entity e) const {
        return world->linear_velocity(registry.get<s::RigidBody>(e).body);
    }

    std::optional<PhysicsWorld> world;
    entt::registry              registry;
};

TEST_F(Gravity, DefaultsToEarth) {
    EXPECT_EQ(s::physics_settings(registry).gravity, glm::vec3(0.0F, -9.81F, 0.0F));
    EXPECT_EQ(world->gravity(), glm::vec3(0.0F, -9.81F, 0.0F));
}

TEST_F(Gravity, TheSceneSettingDrivesJolt) {
    s::physics_settings_mut(registry).gravity = glm::vec3(1.0F, -2.0F, 0.0F);
    const entt::entity ball = add_ball(glm::vec3(0.0F, 50.0F, 0.0F));
    frames(121); // first frame builds the body; then exactly 1 s
    EXPECT_EQ(world->gravity(), glm::vec3(1.0F, -2.0F, 0.0F));
    EXPECT_NEAR(velocity(ball).y, -2.0F, 0.05F);
    EXPECT_NEAR(velocity(ball).x, 1.0F, 0.05F);
}

TEST_F(Gravity, CharactersFallWithTheSameGravity) {
    s::physics_settings_mut(registry).gravity = glm::vec3(0.0F, -2.0F, 0.0F);
    const entt::entity c = place(glm::vec3(0.0F, 50.0F, 0.0F));
    registry.emplace<s::CharacterController>(c);
    frames(60); // 0.5 s in the air
    EXPECT_NEAR(registry.get<s::CharacterController>(c).velocity.y, -1.0F, 0.05F)
        << "the controller still integrates its own -9.81";
}

TEST_F(Gravity, AChangeWakesRestingBodies) {
    add_floor();
    const entt::entity ball = add_ball(glm::vec3(0.0F, 0.5F, 0.0F));
    frames(400); // settle and fall asleep
    ASSERT_EQ(world->active_body_count(), 0U);
    s::physics_settings_mut(registry).gravity = glm::vec3(0.0F, 5.0F, 0.0F);
    frames(60);
    EXPECT_GT(registry.get<s::Transform>(ball).world[3].y, 1.0F) << "the sleeping ball ignored it";
}

TEST(PhysicsSettingsJson, RoundTripsAndValidates) {
    s::PhysicsSettings settings;
    ASSERT_OK(s::apply_physics_settings_json(settings, {{"gravity", {0, -1.62, 0}}}, true));
    EXPECT_FLOAT_EQ(settings.gravity.y, -1.62F);
    const nlohmann::json encoded = s::encode_physics_settings(settings);
    s::PhysicsSettings decoded;
    ASSERT_OK(s::apply_physics_settings_json(decoded, encoded, true));
    EXPECT_EQ(decoded.gravity, settings.gravity);

    EXPECT_FALSE(s::apply_physics_settings_json(settings, {{"gravity", {0, -2000, 0}}}, true));
    EXPECT_FALSE(s::apply_physics_settings_json(settings, {{"gravity", {0, 1}}}, true));
    EXPECT_FALSE(s::apply_physics_settings_json(settings, {{"gravitee", {0, 1, 0}}}, true));
    EXPECT_FLOAT_EQ(settings.gravity.y, -1.62F) << "a failed decode changed the settings";
    ASSERT_OK(s::apply_physics_settings_json(settings, {{"version", 1}, {"future", true}}, false));
}

} // namespace
